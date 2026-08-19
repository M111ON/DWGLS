#!/usr/bin/env python3
import json, urllib.request, pathlib, sys
sys.path.insert(0, r'I:\DWGLS-native-fs\cloud-memory\scripts')
from cloud_memory_push import chunk_text

content = pathlib.Path(r'I:\OpenCode\chat_exports\20260606_063342_New_session_-_2026-06-05T233342671Z.md').read_text(encoding='utf-8')
chunks = chunk_text(content, chunk_size=300)
ch = chunks[1]
print("Chunk 1: %d chars" % len(ch))
print("Content (repr):")
print(repr(ch[:500]))
print("...")
# Try first 800 chars only
t800 = ch[:800]
payload = json.dumps({"input": [t800], "model": "bge-m3"}).encode("utf-8")
req = urllib.request.Request("http://localhost:11434/v1/embeddings", data=payload, headers={"Content-Type": "application/json"})
try:
    resp = urllib.request.urlopen(req, timeout=30)
    result = json.loads(resp.read())
    print("800 chars OK dim=%d" % len(result["data"][0]["embedding"]))
except Exception as e:
    print("800 chars FAIL: %s" % e)
# Try 1000 chars
t1000 = ch[:1000]
payload = json.dumps({"input": [t1000], "model": "bge-m3"}).encode("utf-8")
req = urllib.request.Request("http://localhost:11434/v1/embeddings", data=payload, headers={"Content-Type": "application/json"})
try:
    resp = urllib.request.urlopen(req, timeout=30)
    result = json.loads(resp.read())
    print("1000 chars OK dim=%d" % len(result["data"][0]["embedding"]))
except Exception as e:
    print("1000 chars FAIL: %s" % e)
