"""Anchor-only selection: does dropping the anchor2 OR clause buy pruning?

Current rule (coverage):  entry is a candidate if anchor OR anchor2 is walked.
Measured consequence (sweep_buckets.py): bucket load 341-860 of 2881 entries,
so 1-2 buckets already cover a third of the index and recall@8=1.0 needs
topb 7/8 = scoring ~100%.  No pruning.

Hypothesis: anchor2 is what pulls foreign members into each walked bucket.
Entries are PARTITIONED by their primary anchor (nearest centroid), so
primary-only selection means "score exactly the union of the walked buckets" —
a clean partition, no overlap.

Measured here, on one index and one ground truth, for both rules:
  - recall@k
  - fraction of index actually scored  (the pruning win)

Bucket ranking for an arbitrary topb needs the server's walked order. The live
server (headroom>=K) walks all K buckets, so its list is the full order and any
prefix is reproducible — no restarts needed.
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

DATA = {}
for q in QS:
    qv = embed(q)
    if len(qv) != DIM:
        raise SystemExit('dim mismatch')
    qn = math.sqrt(sum(x * x for x in qv)) or 1.0
    scored = []
    for sid, v, n, a1, a2 in ENTS:
        if n <= 0:
            continue
        dot = sum(qv[i] * v[i] for i in range(DIM))
        scored.append((dot / (qn * n), sid, a1, a2))
    scored.sort(key=lambda t: -t[0])
    walked = server(q, 8)['route']['buckets']
    DATA[q] = (scored, walked)

print('index %d entries, K=8 buckets' % N)
print()
for topk in (3, 8):
    for mode in ('anchor OR anchor2 (now)', 'anchor only (candidate)'):
        use2 = mode.startswith('anchor OR')
        print('=== topk=%d, %s ===' % (topk, mode))
        print('%-6s %-12s %-12s' % ('topb', 'recall', '%scored'))
        for topb in range(1, 9):
            tot = 0.0
            kept = 0
            for q in QS:
                scored, walked = DATA[q]
                truth = [s for _, s, _, _ in scored[:topk]]
                wset = set(walked[:topb])
                cand = [(sc, sid) for sc, sid, a1, a2 in scored
                        if a1 in wset or (use2 and a2 in wset)]
                cand.sort(key=lambda t: -t[0])
                sel = [sid for _, sid in cand[:topk]]
                tot += len(set(truth) & set(sel)) / topk
                kept += len(cand)
            print('%-6d %-12.3f %-12.1f'
                  % (topb, tot / len(QS), 100.0 * (kept / len(QS)) / N))
        print()

# what does anchor-only cost in recall if it is cheaper? and the reverse:
# at recall parity, how much less does anchor-only score?
print('=== summary: topb needed for recall = 1.000 ===')
for topk in (3, 8):
    row = []
    for mode, use2 in (('OR', True), ('only', False)):
        need = None
        for topb in range(1, 9):
            tot = 0.0
            for q in QS:
                scored, walked = DATA[q]
                truth = [s for _, s, _, _ in scored[:topk]]
                wset = set(walked[:topb])
                cand = sorted(((sc, sid) for sc, sid, a1, a2 in scored
                               if a1 in wset or (use2 and a2 in wset)),
                              key=lambda t: -t[0])
                sel = [sid for _, sid in cand[:topk]]
                tot += len(set(truth) & set(sel)) / topk
            if tot / len(QS) >= 0.999:
                need = topb
                break
        pct = None
        if need:
            kept = 0
            for q in QS:
                scored, walked = DATA[q]
                wset = set(walked[:need])
                kept += sum(1 for _, _, a1, a2 in scored
                            if a1 in wset or (use2 and a2 in wset))
            pct = 100.0 * (kept / len(QS)) / N
        row.append('topk=%d %s: topb=%s scored=%.1f%%'
                   % (topk, mode, need, pct if pct is not None else -1))
    print('  ' + ' | '.join(row))