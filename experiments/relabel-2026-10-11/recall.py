#!/usr/bin/env python3
"""relabel recall driver — THE re-label experiment (handoff 2026-10-10 #2).

Question: entries re-anchored under a NON-trained table — does walk recall
jump toward the trained tier (label lottery confirmed, tables first-class)
or stay flat (partition itself at fault)?

Protocol (matches S3 heat): BUCKETS=5 / BUDGET=8000 / PRUNE=1 / FLOOR=0.5,
restart per table, exact frozen truth. Truth = numpy brute cosine top-5 of
each query's STORED vec over all index vecs (frozen to truth.json, reused).
Walk = scratch server POST /v1/state/search topk=5 (query TEXT embedded by
the shared :18095 sidecar — same handicap for every condition).

Conditions: trained (control) | v0n relabeled | jpc relabeled.
Metrics: top1/210, overlap5 (mean |walk top5 cap truth top5| / 5), route modes.

Never touches live :8096 or repo data. Everything under I:\\tmp\\ann_relabel.
Usage: python recall.py [--conditions trained,v0n,jpc] [--n 70]
"""
import json
import os
import random
import shutil
import socket
import sqlite3
import subprocess
import sys
import time
import urllib.request

import numpy as np

WORK = r'I:\tmp\ann_relabel'
ANN = r'I:\tools\ann'
EXE = os.environ.get('ANN_EXE', ANN + '/build/ann_search.exe')
STORE = r'I:\tools\cloud_ws\memcore\fact_store.v2.sqlite3'
SEED = 20261011
KINDS = ('exchange', 'chatlog', 'dictionary_entry')
PROTO_ENV = {
    'LZ_ROUTE_BUCKETS': '5',
    'LZ_SEARCH_BUDGET': '8000',
    'ANN_WALK_PRUNE': '1',
    'ANN_WALK_FLOOR': '0.5',
}


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_health(port, timeout=90.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with urllib.request.urlopen(
                    'http://127.0.0.1:%d/health' % port, timeout=10) as r:
                o = json.loads(r.read())
                if o.get('status') == 'ok':
                    return o
        except Exception:
            pass
        time.sleep(1.0)
    return None


def kill_listener(port):
    """The .bat wrapper orphans the exe on terminate — kill whoever owns
    the listen socket directly (never by image name: live shares it)."""
    try:
        out = subprocess.run(
            ['netstat', '-ano'], capture_output=True, text=True,
            timeout=30).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 5 and parts[1].endswith(':%d' % port) and \
                    parts[3] == 'LISTENING':
                subprocess.run(['taskkill', '/F', '/PID', parts[4]],
                               capture_output=True, timeout=30)
    except Exception as e:
        print('kill_listener :%d: %s' % (port, e))


def spawn(idx, anch, order, port):
    pairs = {
        'ANN_INDEX_PATH': idx,
        'ANN_ANCH_PATH': anch,
        'ANN_ORDER_PATH': order,
        'ANN_ROUTE_LOG': os.path.join(WORK, 'route_%d.log' % port),
        'ANN_PORT': str(port),
        'LZ_EMBED_HOST': '127.0.0.1',
        'LZ_EMBED_PORT': '18095',
    }
    pairs.update(PROTO_ENV)
    bat = os.path.join(WORK, 'run_%d.bat' % port)
    with open(bat, 'w', encoding='utf-8') as f:
        f.write('@echo off\r\n')
        for k, v in pairs.items():
            f.write('set "%s=%s"\r\n' % (k, v))
        f.write('"%s"\r\n' % EXE.replace('\\', '/'))
    p = subprocess.Popen([bat], cwd=ANN,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    h = wait_health(port)
    if h is None:
        p.terminate()
        raise SystemExit('scratch server :%d never healthy' % port)
    return p, h


def sanitize(t):
    """Same folds as scripts/build_index.py: the e5 sidecar 500s on some
    compat chars, so what cannot be embedded cannot be walked (503)."""
    import unicodedata
    t = unicodedata.normalize('NFKC', t)
    for bad, good in (
            ('\u2209', ' not in '), ('\u220b', ' contains '),
            ('\u2216', ' minus '), ('\u221d', ' proportional to '),
            ('\u2224', ' does not divide '), ('\u2245', ' ~= '),
            ('\u22c0', ' AND '), ('\u22c1', ' OR '),
            ('\u23ed', ' next '), ('\u23f8', ' pause '),
            ('\u27fa', ' <=> '), ('\u2b20', ' pentagon ')):
        if bad in t:
            t = t.replace(bad, good)
    return t


def search(port, text, topk=5, vec=None):
    if vec is not None:
        body = json.dumps({'vec': vec, 'topk': topk}).encode()
    else:
        body = json.dumps({'query': sanitize(text[:1000]), 'topk': topk}).encode()
    req = urllib.request.Request('http://127.0.0.1:%d/v1/state/search' % port,
                                 data=body,
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read())


def sample_queries(n_per_kind):
    con = sqlite3.connect('file:%s?mode=ro' % STORE, uri=True, timeout=10)
    try:
        out = []
        rng = random.Random(SEED)
        for kind in KINDS:
            ids = [r[0] for r in con.execute(
                'SELECT id FROM entries WHERE kind=? ORDER BY id', (kind,))]
            pick = rng.sample(ids, n_per_kind)
            for pid in pick:
                row = con.execute(
                    'SELECT id, kind, content FROM entries WHERE id=?',
                    (pid,)).fetchone()
                out.append({'id': row[0], 'kind': row[1],
                            'text': row[2] or ''})
        return out
    finally:
        con.close()


def load_index_vecs(path):
    sids, vecs = [], []
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            o = json.loads(line)
            sids.append(o['sid'])
            vecs.append(o['vec'])
    X = np.asarray(vecs, dtype=np.float32)
    Xn = X / np.linalg.norm(X, axis=1, keepdims=True)
    return sids, Xn


def build_truth(queries, sids, Xn, path):
    if os.path.exists(path):
        t = json.load(open(path, encoding='utf-8'))
        print('truth: loaded frozen %s (%d queries)' % (path, len(t)))
        return t
    by_sid = {s: i for i, s in enumerate(sids)}
    Q = []
    for q in queries:
        if q['id'] not in by_sid:
            raise SystemExit('query id not in index: %s' % q['id'])
        Q.append(Xn[by_sid[q['id']]])
    Q = np.asarray(Q)
    S = Q @ Xn.T
    # nearest NON-self: self-match (cos 1.0) is degenerate for a memory
    # system — recall = does the walk agree with brute on the neighbour.
    S[np.arange(len(Q)), [by_sid[q['id']] for q in queries]] = -np.inf
    top5 = np.argsort(-S, axis=1, kind='stable')[:, :5]
    out = []
    for q, tt in zip(queries, top5):
        out.append({'id': q['id'], 'kind': q['kind'], 'text': q['text'],
                    'truth': [sids[j] for j in tt]})
    json.dump(out, open(path, 'w', encoding='utf-8'))
    print('truth: froze %d queries -> %s' % (len(out), path))
    return out


def run_condition(name, idx, anch, order, truth, vecs_by_id=None):
    port = free_port()
    p, h = spawn(idx, anch, order, port)
    print('  [%s] :%d entries=%s routed=%s' %
          (name, port, h.get('entries'), h.get('routed')), flush=True)
    assert int(h.get('routed')) == 1, 'condition %s unrouted!' % name
    top1 = 0
    self1 = 0
    ov = 0.0
    scored_n = 0
    modes = {}
    per_kind = {}
    t0 = time.time()
    for i, q in enumerate(truth):
        try:
            o = search(port, q['text'],
                       vec=vecs_by_id.get(q['id']) if vecs_by_id else None)
        except Exception as e:
            print('  [%s] q%d error %s' % (name, i, e))
            continue
        scored_n += 1
        got = [x.get('sid') for x in o.get('hits', [])]
        rt = o.get('route', {})
        modes[rt.get('mode')] = modes.get(rt.get('mode'), 0) + 1
        if got[:1] == q['truth'][:1]:
            top1 += 1
        if got[:1] == [q['id']]:
            self1 += 1
        ov += len(set(got[:5]) & set(q['truth'][:5])) / 5.0
        k = per_kind.setdefault(q['kind'], [0, 0])
        k[1] += 1
        if got[:1] == q['truth'][:1]:
            k[0] += 1
    n = len(truth)
    p.terminate()
    kill_listener(port)
    p.wait(timeout=30)
    print('  [%s] nbr_top1=%d/%d (%.1f%%) overlap5=%.3f self_top1=%d '
          'kinds=%s modes=%s (%.0fs, %d scored)' %
          (name, top1, n, 100.0 * top1 / n, ov / n, self1,
           {k: '%d/%d' % tuple(v) for k, v in per_kind.items()}, modes,
           time.time() - t0, scored_n), flush=True)
    return {'nbr_top1': top1, 'n': n, 'overlap5': ov / n, 'self_top1': self1,
            'modes': modes}


def main():
    conds = sys.argv[1].split(',') if len(sys.argv) > 1 else \
        ['trained', 'v0n', 'jpc']
    n_per = int(sys.argv[2]) if len(sys.argv) > 2 else 70
    files = {
        'trained': (WORK + '\\base_index.jsonl', WORK + '\\trained.bin',
                    WORK + '\\trained.order'),
        'v0n': (WORK + '\\v0n_index.jsonl', WORK + '\\v0n.bin',
                WORK + '\\v0n.order'),
        'jpc': (WORK + '\\jpc_index.jsonl', WORK + '\\jpc.bin',
                WORK + '\\jpc.order'),
    }
    print('loading index vecs...', flush=True)
    sids, Xn = load_index_vecs(WORK + '\\base_index.jsonl')
    print('entries:', len(sids), flush=True)
    queries = sample_queries(n_per)
    truth = build_truth(queries, sids, Xn, WORK + '\\truth.json')
    vecs_by_id = None
    if '--vec' in sys.argv:
        print('mode: exact stored vec (no sidecar)', flush=True)
        vecs_by_id = {}
        with open(WORK + '\\base_index.jsonl', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                o = json.loads(line)
                vecs_by_id[o['sid']] = o['vec']
    res = {}
    for c in conds:
        idx, anch, order = files[c]
        for f in (idx, anch, order):
            assert os.path.exists(f), f
        res[c] = run_condition(c, idx, anch, order, truth, vecs_by_id)
    print(json.dumps(res, indent=1))


if __name__ == '__main__':
    main()
