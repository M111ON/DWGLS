#!/usr/bin/env python3
import json, urllib.request, pathlib

c = pathlib.Path(r'I:\OpenCode\chat_exports\20260606_063342_New_session_-_2026-06-05T233342671Z.md').read_text(encoding='utf-8')

for sz in [800, 1000, 1200, 1400, 1600, 1800, 2000]:
    t = c[:sz]
    payload = json.dumps({"input": [t], "model": "bge-m3"}).encode("utf-8")
    req = urllib.request.Request("http://localhost:11434/v1/embeddings", data=payload, headers={"Content-Type": "application/json"})
    try:
        resp = urllib.request.urlopen(req, timeout=60)
        res = json.loads(resp.read())
        print(f"{sz} chars -> OK dim={len(res['data'][0]['embedding'])}")
    except Exception as e:
        print(f"{sz} chars -> FAIL: {e}")
