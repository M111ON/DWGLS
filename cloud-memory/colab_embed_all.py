#!/usr/bin/env python3
"""
colab_embed_all.py — Self-contained: embed all 303 files and push to Cloudflare.
No subprocess calls — direct embedding to avoid output buffering.
"""

import gc
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

CF_MEMORY_URL = os.environ.get("CF_MEMORY_URL", "https://cloud-memory-worker.aexid03.workers.dev")
CF_MEMORY_KEY = os.environ.get("CF_MEMORY_KEY", "")
CHUNK_SIZE = 300
CHARS_PER_TOKEN = 4
EMBED_BATCH = 64  # T4 16GB can handle large batches


def log(msg):
    print(msg, flush=True)


def sanitize_for_embed(text):
    text = text.replace('\x00', '')
    text = re.sub(r'[\x01-\x08\x0b\x0c\x0e-\x1f\x7f]', '', text)
    text = text.replace('\\"', '"').replace("\\'", "'")
    text = re.sub(r' {3,}', '  ', text)
    text = re.sub(r'\n{3,}', '\n\n', text)
    return text.strip()


def chunk_text(text):
    text = sanitize_for_embed(text)
    chunk_chars = CHUNK_SIZE * CHARS_PER_TOKEN
    paragraphs = re.split(r'\n\s*\n', text)
    chunks = []
    current = ""
    for para in paragraphs:
        para = para.strip()
        if not para:
            continue
        if len(current) + len(para) + 2 <= chunk_chars:
            current = current + "\n\n" + para if current else para
        else:
            if current:
                chunks.append(current.strip())
            if len(para) > chunk_chars:
                sentences = re.split(r'(?<=[.!?。！？\n])\s+', para)
                current = ""
                for sent in sentences:
                    if len(current) + len(sent) + 1 <= chunk_chars:
                        current = current + " " + sent if current else sent
                    else:
                        if current:
                            chunks.append(current.strip())
                        current = sent
            else:
                current = para
    if current.strip():
        chunks.append(current.strip())
    return chunks


def push_to_cloudflare(records):
    batch_size = 200
    total = 0
    for i in range(0, len(records), batch_size):
        batch = records[i:i+batch_size]
        payload = json.dumps(batch).encode("utf-8")
        req = urllib.request.Request(
            f"{CF_MEMORY_URL}/ingest",
            data=payload,
            headers={
                "Content-Type": "application/json",
                "X-API-Key": CF_MEMORY_KEY,
                "User-Agent": "colab-embed/1.0",
            },
            method="POST"
        )
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                result = json.loads(resp.read().decode("utf-8"))
                total += result.get("inserted", 0)
        except Exception as e:
            log(f"  [ERROR] Ingest batch {i}: {e}")
        if (i // batch_size) % 10 == 0:
            log(f"  Pushed {total}/{len(records)}")
    return total


def main():
    import hashlib

    if len(sys.argv) < 2:
        log("Usage: python colab_embed_all.py <dir> [--limit N]")
        sys.exit(1)

    chat_dir = Path(sys.argv[1])
    limit = 0
    prefix = ""
    if "--limit" in sys.argv:
        idx = sys.argv.index("--limit")
        if idx + 1 < len(sys.argv):
            limit = int(sys.argv[idx + 1])
    if "--prefix" in sys.argv:
        idx = sys.argv.index("--prefix")
        if idx + 1 < len(sys.argv):
            prefix = sys.argv[idx + 1]

    if not CF_MEMORY_KEY:
        log("Error: Set CF_MEMORY_KEY")
        sys.exit(1)

    # Load model once
    from sentence_transformers import SentenceTransformer
    import torch

    device = "cuda" if torch.cuda.is_available() else "cpu"
    log(f"Device: {device}")
    model = SentenceTransformer("BAAI/bge-m3", device=device)
    log("Model loaded")

    files = sorted(chat_dir.glob("*.md"))
    if limit > 0:
        files = files[:limit]

    log(f"Processing {len(files)} files")

    # Phase 1: Read and chunk all files
    all_chunks = []
    all_meta = []
    for f in files:
        text = f.read_text(encoding="utf-8", errors="replace")
        if len(text.strip()) < 50:
            continue
        chunks = chunk_text(text)
        for ci, chunk in enumerate(chunks):
            all_chunks.append(chunk)
            all_meta.append((prefix + f.name, ci))

    log(f"Total chunks: {len(all_chunks)}")

    # Phase 2: Embed all chunks in large batches (much faster than per-file)
    all_vecs = []
    failed = 0
    for bs in range(0, len(all_chunks), EMBED_BATCH):
        batch = all_chunks[bs:bs+EMBED_BATCH]
        try:
            with torch.no_grad():
                embs = model.encode(batch, show_progress_bar=False, normalize_embeddings=True)
            all_vecs.extend(embs.tolist())
            del embs
        except Exception as e:
            log(f"  [WARN] OOM at batch {bs}: {e}")
            all_vecs.extend([[0.0]*1024]*len(batch))
            failed += len(batch)
        if (bs // EMBED_BATCH) % 20 == 0:
            log(f"  Embedded {min(bs+EMBED_BATCH, len(all_chunks))}/{len(all_chunks)}")
        gc.collect()
        torch.cuda.empty_cache()

    log(f"Embedding done: {len(all_vecs)} vectors, {failed} failed")

    # Phase 3: Push all to Cloudflare in one go
    records = []
    for i, ((fname, cidx), vec) in enumerate(zip(all_meta, all_vecs)):
        cid = hashlib.sha256(f"{fname}::{cidx}::{all_chunks[i][:100]}".encode()).hexdigest()[:16]
        records.append({"id": cid, "source_file": fname, "chunk_index": cidx, "text": all_chunks[i], "vector": vec})

    total_pushed = push_to_cloudflare(records)
    log(f"\nDONE: {total_pushed}/{len(records)} chunks from {len(files)} files, {failed} failed")


if __name__ == "__main__":
    main()
