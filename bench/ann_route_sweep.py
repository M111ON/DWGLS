"""Sweep bucket headroom WITHOUT server restarts.

The fix under test: walk `topb` anchors instead of `topk`.
The live server returns the walked bucket list IN ORDER, and entries carry
their own (anchor, anchor2), so any smaller topb is reproducible exactly by
restricting to walked[:topb]. That gives a real headroom sweep on one index,
one query set, one ground truth.

Reports recall and the fraction of the index that survives pruning — the
whole point of the accelerator is to NOT score everything.
"""
import json, urllib.request, math

ENTS = []
for line in open('build/kv_index.jsonl', encoding='utf-8'):
    d = json.loads(line)
    v = d['vec']
    n = math.sqrt(sum(x * x for x in v))
    ENTS.append((d['sid'], v, n, d.get('anchor', -1), d.get('anchor2', -1)))
DIM = len(ENTS[0][1])
N = len(ENTS)


def embed(q):
    b = json.dumps({'content': q}).encode()
    r = urllib.request.Request('http://127.0.0.1:18095/embedding', data=b,
                               headers={'Content-Type': 'application/json'})
    e = json.loads(urllib.request.urlopen(r, timeout=30).read().decode())[0]['embedding']
    while e and isinstance(e[0], list):
        e = e[0]
    return e


def server(q, k):
    b = json.dumps({'query': q, 'topk': k}).encode()
    r = urllib.request.Request('http://127.0.0.1:8096/v1/state/search', data=b,
                               headers={'Content-Type': 'application/json'})
    return json.loads(urllib.request.urlopen(r, timeout=30).read().decode())


QS = ['tesspack format spec', 'kv remap hybrid rail', 'geofs multivolume',
      'kiss timeline scale', 'moe expert routing', 'wang latch fog',
      'platonic field 20736', 'wang latch fog gate']

# prepare per-query data once
DATA = {}
for q in QS:
    qv = embed(q)
    qn = math.sqrt(sum(x * x for x in qv)) or 1.0
    scored = []
    for sid, v, n, a1, a2 in ENTS:
        if n <= 0:
            continue
        dot = sum(qv[i] * v[i] for i in range(DIM))
        scored.append((dot / (qn * n), sid, a1, a2))
    scored.sort(key=lambda t: -t[0])
    walked = server(q, 8)['route']['buckets']
    live8 = [h['sid'] for h in server(q, 8)['hits']]
    DATA[q] = (scored, walked, live8)

K = 8
print('index=%d entries, %d buckets walked by the live server' % (N, K))
print()
for topk in (3, 8):
    print('=== topk=%d ===' % topk)
    print('%-10s %-10s %-10s' % ('topb', 'recall@%d' % topk, 'scored%'))
    for topb in range(1, K + 1):
        tot = 0.0
        kept = 0
        for q in QS:
            scored, walked, _ = DATA[q]
            truth = [s for _, s, _, _ in scored[:topk]]
            wset = set(walked[:topb])
            cand = [(sc, sid) for sc, sid, a1, a2 in scored
                    if a1 in wset or a2 in wset]
            cand.sort(key=lambda t: -t[0])
            sel = [sid for _, sid in cand[:topk]]
            tot += len(set(truth) & set(sel)) / topk
            kept += len(cand)
        r = tot / len(QS)
        pct = 100.0 * (kept / len(QS)) / N
        print('%-10d %-10.3f %-10.1f' % (topb, r, pct))
    print()

# cross-check: live server at topk=8 (headroom 4 -> topb=8) must equal sim topb=8
ok = True
for q in QS:
    scored, walked, live8 = DATA[q]
    wset = set(walked[:8])
    cand = sorted(((sc, sid) for sc, sid, a1, a2 in scored
                   if a1 in wset or a2 in wset), key=lambda t: -t[0])
    sim = [sid for _, sid in cand[:8]]
    if set(sim) != set(live8):
        ok = False
        print('MISMATCH', q, len(set(sim) ^ set(live8)))
print('sim(topb=8) == live server hits:', ok)