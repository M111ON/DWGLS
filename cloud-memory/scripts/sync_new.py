#!/usr/bin/env python3
"""
sync_new.py — Incremental sync: embed ONLY files not yet in Cloud Memory.

Scans chat export folders, fetches the indexed source list from the Worker,
and pushes only new/changed files. Auto-starts local llama-server if needed.

Usage:
    python sync_new.py
    python sync_new.py --dry-run
    python sync_new.py --api-url URL --api-key KEY

Folders to scan (prefix -> path):
    (no prefix)  I:/OpenCode/chat_exports   -> opencode sessions
    hm_          I:/Hermes/chat_exports     -> Hermes sessions
    vault_       I:/Vaults                  -> Obsidian vault notes
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

DEFAULT_API_URL = "https://cloud-memory-worker.aexid03.workers.dev"
LIST_SOURCES = DEFAULT_API_URL + "/list-sources"
INGEST_URL = DEFAULT_API_URL + "/ingest"

EMBED_URL = "http://127.0.0.1:11434/v1/embeddings"
MODELS_URL = "http://127.0.0.1:11434/v1/models"
EMBED_MODEL = "bge-m3"
EMBED_DIM = 1024

LLAMA_SERVER = r"I:\llama\llama-b9733-bin-win-vulkan-x64\llama-server.exe"
LLAMA_MODEL = r"I:\llama\models\bge-m3-q8_0.gguf"
SERVER_LOG = r"I:\llama\models\embed-sync.log"

CHUNK_SIZE = 300        # tokens
CHARS_PER_TOKEN = 4
EMBED_BATCH = 16        # chunks per embed request

# folder -> source prefix
FOLDERS = [
    (Path(r"I:\OpenCode\chat_exports"), ""),
    (Path(r"I:\Hermes\chat_exports"), "hm_"),
    (Path(r"I:\Vaults\Memory"), "vault_Memory/"),
    (Path(r"I:\Vaults\Inbox"), "vault_Inbox/"),
    (Path(r"I:\Vaults\Archive"), "vault_Archive/"),
    (Path(r"I:\Vaults\Members"), "vault_Members/"),
    (Path(r"I:\Vaults\Projects"), "vault_Projects/"),
    (Path(r"I:\Vaults\Research"), "vault_Research/"),
]


def log(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------------------
# embedding server
# ---------------------------------------------------------------------------
def server_alive() -> bool:
    try:
        req = urllib.request.Request(MODELS_URL, method="GET")
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status == 200
    except Exception:
        return False


def start_server():
    if not (os.path.exists(LLAMA_SERVER) and os.path.exists(LLAMA_MODEL)):
        log(f"[ERR] llama-server/model not found:\n  {LLAMA_SERVER}\n  {LLAMA_MODEL}")
        return False
    try:
        with open(SERVER_LOG, "ab"), open(SERVER_LOG + ".err", "ab"):
            pass
        p = subprocess.Popen(
            [LLAMA_SERVER, "-m", LLAMA_MODEL, "--embedding",
             "--embd-normalize", "2", "--port", "11434",
             "--host", "127.0.0.1", "-c", "8192", "-ub", "4096",
             "--no-warmup"],
            stdout=open(SERVER_LOG, "a"), stderr=open(SERVER_LOG + ".err", "a"),
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        log(f"  llama-server started (PID {p.pid}) — waiting warmup...")
        for _ in range(120):
            time.sleep(1)
            if server_alive():
                return True
        log("[ERR] server did not become ready in 120s")
        return False
    except Exception as e:
        log(f"[ERR] could not start llama-server: {e}")
        return False


def ensure_server():
    if server_alive():
        log("  llama-server already running")
        return True
    log("  starting llama-server...")
    return start_server()


# ---------------------------------------------------------------------------
# sanitize / chunk
# ---------------------------------------------------------------------------
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


# ---------------------------------------------------------------------------
# embed (batched)
# ---------------------------------------------------------------------------
def embed_batch(texts):
    """Embed a batch of texts via llama.cpp. Returns list of vectors or None per failed."""
    vectors = []
    for i in range(0, len(texts), EMBED_BATCH):
        batch = texts[i:i+EMBED_BATCH]
        payload = json.dumps({"input": batch, "model": EMBED_MODEL}).encode("utf-8")
        for attempt in range(3):
            try:
                req = urllib.request.Request(
                    EMBED_URL, data=payload,
                    headers={"Content-Type": "application/json"},
                    method="POST")
                with urllib.request.urlopen(req, timeout=120) as resp:
                    result = json.loads(resp.read().decode("utf-8"))
                # data may be flat or nested depending on server version
                data = result.get("data", [])
                for item in data:
                    emb = item["embedding"]
                    if isinstance(emb, list) and emb and isinstance(emb[0], list):
                        vectors.append(emb[0])
                    else:
                        vectors.append(emb)
                break
            except Exception as e:
                if attempt < 2:
                    time.sleep(1 + attempt)
                else:
                    log(f"  [ERROR] embed batch {i} failed: {e}")
                    raise SystemExit(f"Embedding server unavailable — aborting instead of pushing zero vectors")
    return vectors


# ---------------------------------------------------------------------------
# push
# ---------------------------------------------------------------------------
def push_records(records, api_url, api_key):
    total = 0
    BATCH = 100
    for i in range(0, len(records), BATCH):
        batch = records[i:i+BATCH]
        payload = json.dumps(batch).encode("utf-8")
        req = urllib.request.Request(
            f"{api_url}/ingest", data=payload,
            headers={"Content-Type": "application/json",
                     "X-API-Key": api_key,
                     "User-Agent": "cloud-memory-sync/1.0"},
            method="POST")
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                result = json.loads(resp.read().decode("utf-8"))
                total += result.get("inserted", 0)
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            log(f"  [ERR] ingest failed ({e.code}): {body}")
            return total
        time.sleep(0.2)
    return total


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Incremental Cloud Memory sync")
    parser.add_argument("--api-url", default=os.environ.get("CF_MEMORY_URL", DEFAULT_API_URL))
    parser.add_argument("--api-key", default=os.environ.get("CF_MEMORY_KEY"))
    parser.add_argument("--dry-run", action="store_true", help="List new files only")
    parser.add_argument("--reindex", default="",
                        help="Re-push files whose fullname starts with this prefix (e.g. 'vault_')")
    args = parser.parse_args()

    if not args.api_key:
        key_file = Path(__file__).parent.parent / ".api_key"
        if key_file.exists():
            args.api_key = key_file.read_text().strip()
        else:
            log("Error: no API key. Set --api-key / CF_MEMORY_KEY or create .api_key")
            sys.exit(1)

    # 1) Fetch indexed sources
    log("Fetching indexed sources from Cloud Memory...")
    req = urllib.request.Request(args.api_url + "/list-sources",
                                 headers={"User-Agent": "cloud-memory-sync/1.0"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        sources = json.loads(resp.read().decode("utf-8"))
    indexed = {s["source_file"] for s in sources}
    log(f"  Indexed: {len(indexed)} sources")

    # 2) Scan local folders for new files
    new_files = []  # (prefix, fullname, path)
    for folder, prefix in FOLDERS:
        if not folder.exists():
            log(f"  [skip] folder not found: {folder}")
            continue
        for f in sorted(folder.rglob("*.md")):
            rel = f.relative_to(folder).as_posix()
            fullname = prefix + rel
            if args.reindex:
                if fullname.startswith(args.reindex):
                    new_files.append((prefix, fullname, f))
            elif fullname not in indexed:
                new_files.append((prefix, fullname, f))
    log(f"  New files to embed: {len(new_files)}")

    if args.dry_run:
        for _, fullname, f in new_files:
            log(f"    {fullname}  ({f.stat().st_size//1024} KB)")
        return

    if not new_files:
        log("\nNothing to sync. All up to date.")
        return

    # 3) Start embed server
    if not ensure_server():
        sys.exit(1)

    # 4) Process each new file
    total_pushed = 0
    total_failed = 0
    for prefix, fullname, f in new_files:
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except Exception as e:
            log(f"\n[ERR] {fullname}: cannot read: {e}")
            total_failed += 1
            continue
        if len(text.strip()) < 50:
            log(f"\n[skip] {fullname}: too short")
            continue

        chunks = chunk_text(text)
        log(f"\n{fullname}: {len(chunks)} chunks")
        if not chunks:
            continue

        vecs = embed_batch(chunks)
        records = []
        for i, (c, v) in enumerate(zip(chunks, vecs)):
            cid = hashlib.sha256(f"{fullname}::{i}::{c[:100]}".encode()).hexdigest()[:16]
            records.append({"id": cid, "source_file": fullname,
                            "chunk_index": i, "text": c, "vector": v})

        n = push_records(records, args.api_url, args.api_key)
        total_pushed += n
        log(f"  -> pushed {n} chunks")

    log(f"\n{'='*50}")
    log(f"Done: {total_pushed} chunks pushed, {total_failed} files failed")


if __name__ == "__main__":
    main()
