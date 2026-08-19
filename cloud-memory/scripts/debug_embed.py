#!/usr/bin/env python3
import json, urllib.request, pathlib, sys
sys.path.insert(0, r'I:\DWGLS-native-fs\cloud-memory\scripts')
from cloud_memory_push import chunk_text

content = pathlib.Path(r'I:\OpenCode\chat_exports\20260606_063342_New_session_-_2026-06-05T233342671Z.md').read_text(encoding='utf-8')
chunks = chunk_text(content, chunk_size=300)
print("Chunks:", len(chunks))
for i, ch in enumerate(chunks):
    print("Chunk %d: %d chars" % (i, len(ch)))
    payload = json.dumps({"input": [ch], "model": "bge-m3"}).encode("utf-8")
    print("  Payload: %d bytes" % len(payload))
    req = urllib.request.Request("http://localhost:11434/v1/embeddings", data=payload, headers={"Content-Type": "application/json"})
    try:
        resp = urllib.request.urlopen(req, timeout=30)
        result = json.loads(resp.read())
        print("  OK dim=%d" % len(result["data"][0]["embedding"]))
    except Exception as e:
        print("  FAIL: %s" % e)
