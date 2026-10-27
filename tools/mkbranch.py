"""Make a doctored copy of a KV log: one delta re-parented to another branch,
with every hash recomputed so the CHAIN still verifies. Used to prove that the
branch rule catches a record the chain cannot: only a parent check can see it.

usage: python mkbranch.py <src dir> <dst dir> <turn whose parent to change> <new parent>
"""
import hashlib, json, os, shutil, sys

src, dst, turn, newp = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
os.makedirs(dst, exist_ok=True)
blob = os.path.join(src, "kv_blobs.bin")
if os.path.exists(blob):
    shutil.copy2(blob, os.path.join(dst, "kv_blobs.bin"))

lines = [json.loads(x) for x in open(os.path.join(src, "log.jsonl"), encoding="utf-8") if x.strip()]
prev = "GENESIS"
hit = 0
for m in lines:
    if m.get("kind") == "kv-delta":
        d = json.loads(m["text"])
        if d["turn"] == turn:
            d["parent"] = newp
            m["text"] = json.dumps(d, separators=(",", ":"))
            hit += 1
    m["prev"] = prev
    m["hash"] = hashlib.sha256(
        (prev + m["app"] + m["kind"] + m["to"] + m["text"]).encode("utf-8")).hexdigest()
    prev = m["hash"]

with open(os.path.join(dst, "log.jsonl"), "w", encoding="utf-8") as f:
    for m in lines:
        f.write(json.dumps(m, ensure_ascii=False) + "\n")
print("re-parented %d record(s) at turn %d -> parent %d, %d lines rewritten"
      % (hit, turn, newp, len(lines)))
