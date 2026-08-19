#!/usr/bin/env python3
"""
colab_embed.py — Run in Colab (GPU) to embed chat exports and push to Cloudflare.
Handles large files by batching embedding in small groups to avoid OOM.
"""

import gc
import json
import os
import re
import sys
import urllib.request
from pathlib import Path
from typing import List

# Config
CF_MEMORY_URL = os.environ.get("CF_MEMORY_URL", "https://cloud-memory-worker.aexid03.workers.dev")
CF_MEMORY_KEY = os.environ.get("CF_MEMORY_KEY", "")
CHUNK_SIZE = 300  # tokens
CHARS_PER_TOKEN = 4
EMBED_BATCH = 16  # chunks per GPU batch


def sanitize_for_embed(text: str) -> str:
    """Remove/replace characters that crash embedding model."""
    text = text.replace('\x00', '')
    text = re.sub(r'[\x01-\x08\x0b\x0c\x0e-\x1f\x7f]', '', text)
    text = text.replace('\\"', '"').replace("\\'", "'")
    text = re.sub(r' {3,}', '  ', text)
    text = re.sub(r'\n{3,}', '\n\n', text)
    return text.strip()


def chunk_text(text: str, chunk_size: int = CHUNK_SIZE) -> List[str]:
    """Split text into chunks."""
    text = sanitize_for_embed(text)
    chunk_chars = chunk_size * CHARS_PER_TOKEN

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


def embed_batch(model, texts: List[str]) -> List[List[float]]:
    """Embed a small batch of texts, clearing GPU memory after."""
    import torch

    embeddings = model.encode(texts, show_progress_bar=False, normalize_embeddings=True)
    result = embeddings.tolist()

    # Force GPU memory cleanup
    del embeddings
    gc.collect()
    torch.cuda.empty_cache()

    return result


def push_to_cloudflare(chunks: List[str], embeddings: List[List[float]], source_file: str):
    """Push chunks and embeddings to Cloudflare Worker."""
    import hashlib

    records = []
    for i, (chunk, vector) in enumerate(zip(chunks, embeddings)):
        chunk_id = hashlib.sha256(f"{source_file}::{i}::{chunk[:100]}".encode()).hexdigest()[:16]
        records.append({
            "id": chunk_id,
            "source_file": source_file,
            "chunk_index": i,
            "text": chunk,
            "vector": vector,
        })

    # Send in batches of 100
    batch_size = 100
    total_inserted = 0
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
            with urllib.request.urlopen(req, timeout=60) as resp:
                result = json.loads(resp.read().decode("utf-8"))
                total_inserted += result.get("inserted", 0)
        except Exception as e:
            print(f"  [ERROR] Ingest failed: {e}")

    return total_inserted


def main():
    if len(sys.argv) < 2:
        print("Usage: python colab_embed.py <chat_exports_dir> [--limit N]")
        sys.exit(1)

    chat_dir = Path(sys.argv[1])
    limit = 0
    if "--limit" in sys.argv:
        idx = sys.argv.index("--limit")
        if idx + 1 < len(sys.argv):
            limit = int(sys.argv[idx + 1])

    if not CF_MEMORY_KEY:
        print("Error: Set CF_MEMORY_KEY environment variable")
        sys.exit(1)

    # Load model ONCE
    from sentence_transformers import SentenceTransformer
    import torch

    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"Using device: {device}")
    model = SentenceTransformer("BAAI/bge-m3", device=device)
    print("Model loaded")

    # Get files
    files = sorted(chat_dir.glob("*.md"))
    if limit > 0:
        files = files[:limit]

    print(f"Processing {len(files)} files from {chat_dir}")

    # Embed and push
    total_chunks = 0
    total_pushed = 0
    failed_chunks = 0

    for f in files:
        text = f.read_text(encoding="utf-8", errors="replace")
        if len(text.strip()) < 50:
            continue

        chunks = chunk_text(text)
        if not chunks:
            continue

        # Embed in small batches to avoid OOM
        all_embeddings = []
        for batch_start in range(0, len(chunks), EMBED_BATCH):
            batch = chunks[batch_start:batch_start + EMBED_BATCH]
            try:
                batch_embs = embed_batch(model, batch)
                all_embeddings.extend(batch_embs)
            except Exception as e:
                print(f"  [WARN] Embed batch failed at {f.name} chunk {batch_start}: {e}")
                # Use zero vectors for failed batch
                all_embeddings.extend([[0.0] * 1024] * len(batch))
                failed_chunks += len(batch)
                gc.collect()
                torch.cuda.empty_cache()

        # Push
        pushed = push_to_cloudflare(chunks, all_embeddings, f.name)
        total_chunks += len(chunks)
        total_pushed += pushed

        print(f"  {f.name}: {len(chunks)} chunks, {pushed} pushed")

    print(f"\nDone: {total_pushed}/{total_chunks} chunks pushed from {len(files)} files")
    if failed_chunks > 0:
        print(f"Failed: {failed_chunks} chunks (zero vectors)")
