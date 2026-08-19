#!/usr/bin/env python3
"""
cloud_memory_push.py — Read markdown chat logs, chunk, embed via local llama.cpp,
and push to Cloudflare Worker (D1 + Vectorize).

Usage:
    python cloud_memory_push.py [FILE_OR_DIR] [--api-url URL] [--api-key KEY] [--chunk-size TOKENS]

Examples:
    # Push a single file
    python cloud_memory_push.py I:/OpenCode/chat_exports/20260608_012245_Search_zone_card_policy_explore_subagent.md

    # Push all .md files in a directory (first 5 only for testing)
    python cloud_memory_push.py I:/OpenCode/chat_exports --limit 5

    # Push with custom API key
    python cloud_memory_push.py I:/OpenCode/chat_exports --api-key YOUR_KEY --limit 3
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path
from typing import List, Dict, Optional

DEFAULT_API_URL = "https://cloud-memory-worker.aexid03.workers.dev"
DEFAULT_CHUNK_SIZE = 300  # tokens (llama.cpp bge-m3 max ~1200 chars ~300 tokens)
EMBED_URL = "http://localhost:11434/v1/embeddings"
EMBED_MODEL = "bge-m3"


def sanitize_for_embed(text: str) -> str:
    """Remove/replace characters that crash llama-server embedding."""
    # Remove null bytes
    text = text.replace('\x00', '')
    # Remove other control chars (keep \n \r \t)
    text = re.sub(r'[\x01-\x08\x0b\x0c\x0e-\x1f\x7f]', '', text)
    # Replace escaped quotes that might break JSON
    text = text.replace('\\"', '"').replace("\\'", "'")
    # Collapse multiple spaces/newlines
    text = re.sub(r' {3,}', '  ', text)
    text = re.sub(r'\n{3,}', '\n\n', text)
    return text.strip()


def chunk_text(text: str, chunk_size: int = 300, overlap: int = 50) -> List[str]:
    """Split text into chunks of approximately chunk_size tokens.
    
    Uses paragraph boundaries first, then falls back to sentence splitting.
    Approximate token count: ~4 chars per token for mixed Thai/English.
    llama.cpp bge-m3 max input ~1200 chars (~300 tokens).
    """
    # Sanitize before chunking
    text = sanitize_for_embed(text)
    chars_per_token = 4
    chunk_chars = chunk_size * chars_per_token
    overlap_chars = overlap * chars_per_token
    
    # Split by double newlines (paragraphs)
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
            # If single paragraph is too large, split by sentences
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


def embed_chunks(chunks: List[str], embed_url: str = EMBED_URL) -> List[List[float]]:
    """Embed chunks via local llama.cpp bge-m3 endpoint."""
    all_embeddings = []
    
    for i, chunk in enumerate(chunks):
        # Sanitize chunk before embedding
        chunk = sanitize_for_embed(chunk)
        if len(chunk.strip()) < 10:
            continue  # Skip empty/tiny chunks
        
        payload = json.dumps({
            "input": [chunk],
            "model": EMBED_MODEL
        }).encode("utf-8")
        
        for attempt in range(3):
            try:
                req = urllib.request.Request(
                    embed_url,
                    data=payload,
                    headers={"Content-Type": "application/json"},
                    method="POST"
                )
                with urllib.request.urlopen(req, timeout=60) as resp:
                    result = json.loads(resp.read().decode("utf-8"))
                    for item in result.get("data", []):
                        all_embeddings.append(item["embedding"])
                    break
            except Exception as e:
                if attempt < 2:
                    time.sleep(1 + attempt)
                else:
                    print(f"  [WARN] Embed chunk {i} failed, skipping")
                    # Skip this chunk entirely (don't add zero vector)
                    chunks[i] = None  # Mark for removal
    
    return all_embeddings


def push_to_cloudflare(
    chunks: List[str],
    embeddings: List[List[float]],
    source_file: str,
    api_url: str,
    api_key: str
) -> int:
    """Push chunks and embeddings to Cloudflare Worker."""
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
            f"{api_url}/ingest",
            data=payload,
            headers={
                "Content-Type": "application/json",
                "X-API-Key": api_key,
                "User-Agent": "cloud-memory-push/1.0",
            },
            method="POST"
        )
        
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                result = json.loads(resp.read().decode("utf-8"))
                total_inserted += result.get("inserted", 0)
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            print(f"  [ERROR] Ingest failed ({e.code}): {body}")
            return total_inserted
        
        time.sleep(0.2)
    
    return total_inserted


def process_file(
    filepath: Path,
    api_url: str,
    api_key: str,
    chunk_size: int,
    dry_run: bool = False
) -> bool:
    """Process a single markdown file: read, chunk, embed, push."""
    print(f"\n{'='*60}")
    print(f"Processing: {filepath.name}")
    
    try:
        text = filepath.read_text(encoding="utf-8", errors="replace")
    except Exception as e:
        print(f"  [ERROR] Cannot read file: {e}")
        return False
    
    if len(text.strip()) < 50:
        print(f"  [SKIP] Too short ({len(text)} chars)")
        return False
    
    # Chunk
    chunks = chunk_text(text, chunk_size=chunk_size)
    print(f"  Chunks: {len(chunks)} (avg {sum(len(c) for c in chunks)//len(chunks)} chars)")
    
    if dry_run:
        print(f"  [DRY RUN] Would embed and push {len(chunks)} chunks")
        return True
    
    # Embed
    print(f"  Embedding {len(chunks)} chunks via local llama.cpp...")
    embeddings = embed_chunks(chunks)
    
    # Filter out failed chunks (None in chunks list)
    valid_pairs = [(c, e) for c, e in zip(chunks, embeddings) if c is not None]
    if len(valid_pairs) < len(chunks):
        print(f"  Skipped {len(chunks) - len(valid_pairs)} failed chunks")
    chunks = [p[0] for p in valid_pairs]
    embeddings = [p[1] for p in valid_pairs]
    
    if not chunks:
        print(f"  [SKIP] No valid chunks after embedding")
        return False
    
    print(f"  Embedded: {len(embeddings)} vectors, dim={len(embeddings[0]) if embeddings else 0}")
    
    # Push
    print(f"  Pushing to Cloudflare...")
    inserted = push_to_cloudflare(chunks, embeddings, filepath.name, api_url, api_key)
    print(f"  ✓ Inserted {inserted} chunks")
    
    return True


def main():
    parser = argparse.ArgumentParser(description="Push chat logs to Cloud Memory")
    parser.add_argument("path", help="File or directory to process")
    parser.add_argument("--api-url", default=os.environ.get("CF_MEMORY_URL", DEFAULT_API_URL))
    parser.add_argument("--api-key", default=os.environ.get("CF_MEMORY_KEY"))
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE)
    parser.add_argument("--limit", type=int, default=0, help="Max files to process (0=all)")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be done")
    args = parser.parse_args()
    
    # Load API key from file if not provided
    if not args.api_key:
        key_file = Path(__file__).parent.parent / ".api_key"
        if key_file.exists():
            args.api_key = key_file.read_text().strip()
        else:
            print("Error: No API key. Set --api-key or CF_MEMORY_KEY env, or create .api_key file")
            sys.exit(1)
    
    target = Path(args.path)
    if target.is_file():
        files = [target]
    elif target.is_dir():
        files = sorted(target.glob("*.md"))
        if args.limit > 0:
            files = files[:args.limit]
    else:
        print(f"Error: {target} not found")
        sys.exit(1)
    
    print(f"Cloud Memory Push")
    print(f"  API: {args.api_url}")
    print(f"  Files: {len(files)}")
    print(f"  Chunk size: ~{args.chunk_size} tokens")
    if args.dry_run:
        print(f"  [DRY RUN]")
    
    success = 0
    failed = 0
    
    for f in files:
        if process_file(f, args.api_url, args.api_key, args.chunk_size, args.dry_run):
            success += 1
        else:
            failed += 1
    
    print(f"\n{'='*60}")
    print(f"Done: {success} succeeded, {failed} failed")


if __name__ == "__main__":
    main()