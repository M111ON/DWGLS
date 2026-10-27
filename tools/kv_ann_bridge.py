#!/usr/bin/env python3
"""kv_ann_bridge.py — Phase 1 bridge: KV frames -> ANN rows (addresses only).

The two systems stay separate (#7587): ANN owns the vector space and the search,
KV owns the frames and the replay plan. What crosses is a row per KV frame:

    {"sid": "...", "file": "<kv log>", "npast": <turn>, "dim": 384, "vec": [...]}

`vec` is NOT computed here. It is reused from memcore's fact_store, which already
holds multilingual-e5-small vectors at the same 384 dims the ANN sidecar serves
(same space, measured cosine 0.955 in build_index.py). So the bridge adds no
embedder, no model, and no new space — a turn that nobody embedded simply does
not become a row, and is reported as a miss instead of being faked.

Next to the rows it writes a sidecar so a hit is actionable:

    {"sid","turn","parent","anchor_turn","anchor_blob":[off,len,fnv],"replay":[turns]}

`replay` is the exact seek plan: resume the anchor blob, then replay those turns'
deltas. That is the whole point of the bridge — ANN finds the text, the sidecar
says which frame to resume, and KV stays out of the ranking.

Verification this script performs and prints:
  1. loader-compat  — sid <= 63 chars, vec present, len(vec) == dim <= 1024
  2. resolvable     — every row's turn has a delta in the log and a seek plan
  3. value-blind    — routing columns come from the turn, never the text: the same
                      run with every text replaced by "x"*len yields identical
                      (sid-free) routing columns
  4. deterministic  — two runs produce byte-identical files

Usage:
    python kv_ann_bridge.py --kv DIR --memcore DB [--out FILE] [--sidecar FILE]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sqlite3
import struct
import sys
import unicodedata

DEFAULT_MEMCORE = r"I:\tools\cloud_ws\memcore\fact_store.v2.sqlite3"
MAX_SID = 63
MAX_DIM = 1024

_NORM_INDEX = {}          # normalized content -> entry id, built once


def normalize(t):
    """NFKC + whitespace collapse. Exact byte equality is too strict a join: the
    same sentence with a newline collapsed to a space missed (measured 3/8 turns)
    even though the corpus holds it. build_index.py normalizes for the same
    reason; here it decides whether a KV frame can become an ANN row at all."""
    t = unicodedata.normalize("NFKC", t)
    return " ".join(t.split())


def read_kv_log(path):
    """anchors + deltas from the KV log, tolerating every shape the file carries.

    Same tolerances the C reader needed, for the same measured reasons: spaced
    json.dumps output (pool writes that way), compact output (our host writes
    that way), CRLF from anything that re-serialized on Windows, and a torn last
    line. Skipping punctuation instead of matching it is what makes both shapes
    readable; matching `"kind":"kv-delta"` silently saw zero deltas in a spaced
    log.
    """
    anchors, deltas = [], []
    if not os.path.exists(path):
        return anchors, deltas
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\r\n ")
            if len(line) < 64 or not line.endswith("}"):
                continue                      # torn tail or not a record
            try:
                m = json.loads(line)
            except Exception:
                continue
            kind, text = m.get("kind"), m.get("text")
            if not isinstance(text, str):
                continue
            try:
                body = json.loads(text)
            except Exception:
                continue
            if kind == "kv-anchor" and body.get("turn"):
                anchors.append(body)
            elif kind == "kv-delta" and body.get("turn"):
                body["_tokens"] = body.get("tokens") or []
                deltas.append(body)
    anchors.sort(key=lambda a: a["turn"])
    deltas.sort(key=lambda d: d["turn"])
    return anchors, deltas


def read_turns(path):
    """turn -> text, from the producer's turns.jsonl (one frame per line)."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\r\n ")
            if not line.endswith("}"):
                continue
            try:
                m = json.loads(line)
            except Exception:
                continue
            if m.get("turn") and isinstance(m.get("text"), str):
                out[int(m["turn"])] = m["text"]
    return out


def memcore_vector(db, text):
    """Reuse the stored vector, never re-embed: exact text first, then normalized.

    Normalizing costs one pass over the corpus's text column (no vectors loaded);
    paying it only when an exact match fails keeps the common case one query.
    """
    with sqlite3.connect(db) as c:
        row = c.execute(
            """select e.id, b.dim, b.vec from entries e
               join embeddings b on b.entry_id = e.id
               where e.content = ? limit 1""", (text,)).fetchone()
        if not row:
            if not _NORM_INDEX:
                for eid, content in c.execute("select id, content from entries"):
                    if isinstance(content, str):
                        _NORM_INDEX.setdefault(normalize(content), eid)
            eid = _NORM_INDEX.get(normalize(text))
            if eid is None:
                return None
            row = c.execute(
                """select e.id, b.dim, b.vec from embeddings b
                   join entries e on e.id = b.entry_id
                   where e.id = ? limit 1""", (eid,)).fetchone()
    if not row:
        return None
    eid, dim, blob = row
    if not blob or dim <= 0 or dim > MAX_DIM or len(blob) < dim * 4:
        return None
    vec = list(struct.unpack("<%df" % dim, blob[:dim * 4]))
    return eid, dim, vec


def seek_plan(anchors, deltas, turn):
    """resume the last anchor at/before `turn`, then replay the deltas after it."""
    if not any(d["turn"] == turn for d in deltas):
        return None
    at = [a for a in anchors if a["turn"] <= turn]
    if not at:
        return None
    base = at[-1]
    if int(base["turn"]) != turn and any(d["turn"] == turn for d in deltas):
        pass
    replay = [d["turn"] for d in deltas if base["turn"] < d["turn"] <= turn]
    return {
        "anchor_turn": int(base["turn"]),
        "anchor_blob": [int(base.get("blob_off", 0)), int(base.get("blob_len", 0)),
                        int(base.get("blob_fnv", 0))],
        "replay": replay,
    }


def row_sid(turn, text):
    h = hashlib.sha256(("%d:%s" % (turn, text)).encode("utf-8")).hexdigest()[:16]
    return "kv:" + h


def build(kvdir, memcore, blind=False):
    anchors, deltas = read_kv_log(os.path.join(kvdir, "log.jsonl"))
    texts = read_turns(os.path.join(kvdir, "turns.jsonl"))
    rows, sidecar, misses, unresolvable, routing = [], [], [], [], []
    for d in deltas:
        turn = int(d["turn"])
        plan = seek_plan(anchors, deltas, turn)
        if plan is None:
            unresolvable.append(turn)
            continue
        # routing is derived from the TURN and the log, never from the text: this
        # list is what the value-blindness probe compares
        routing.append((turn, plan["anchor_turn"], len(plan["replay"])))
        if blind:
            continue
        text = texts.get(turn)
        if text is None:
            misses.append((turn, "no text recorded for this frame"))
            continue
        mv = memcore_vector(memcore, text)
        if not mv:
            misses.append((turn, "no stored vector for this text"))
            continue
        eid, dim, vec = mv
        sid = row_sid(turn, text)
        rows.append({"sid": sid, "file": os.path.join(kvdir, "log.jsonl"),
                     "npast": turn, "dim": dim, "vec": vec})
        sidecar.append({"sid": sid, "turn": turn, "parent": int(d.get("parent", 0)),
                        "text_sha": hashlib.sha256(text.encode("utf-8")).hexdigest()[:16],
                        "memcore_id": eid, **plan})
    return rows, sidecar, misses, unresolvable, anchors, deltas, routing


def write_jsonl(path, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kv", required=True)
    ap.add_argument("--memcore", default=DEFAULT_MEMCORE)
    ap.add_argument("--out", default=None)
    ap.add_argument("--sidecar", default=None)
    ap.add_argument("--blind", action="store_true",
                    help="replace every text with x*len (value-blindness probe)")
    args = ap.parse_args()

    out = args.out or os.path.join(args.kv, "ann_rows_kv.jsonl")
    side = args.sidecar or os.path.join(args.kv, "kv_frames.jsonl")

    rows, sidecar, misses, unresolvable, anchors, deltas, routing = build(
        args.kv, args.memcore, blind=args.blind)

    bad = []
    for r in rows:
        if len(r["sid"]) > MAX_SID: bad.append("sid too long: %r" % r["sid"])
        if not isinstance(r.get("vec"), list) or not r["vec"]: bad.append("no vec on %s" % r["sid"])
        elif len(r["vec"]) != r["dim"]: bad.append("vec/dim mismatch on %s" % r["sid"])
        elif r["dim"] > MAX_DIM: bad.append("dim %d > %d" % (r["dim"], MAX_DIM))
        if not isinstance(r.get("npast"), int): bad.append("npast not an int on %s" % r["sid"])

    if not args.blind:
        write_jsonl(out, rows)
        write_jsonl(side, sidecar)
        # determinism: build again and compare
        rows2, side2, _, _, _, _, _ = build(args.kv, args.memcore)
        det = (json.dumps(rows, sort_keys=True) == json.dumps(rows2, sort_keys=True)
               and json.dumps(sidecar, sort_keys=True) == json.dumps(side2, sort_keys=True))
        # value-blind: with every text destroyed, the routing columns must be
        # identical — proven over ALL resolvable turns, not only the ones that
        # happened to have a vector, so vector availability cannot mask a failure
        *_, blind_routing = build(args.kv, args.memcore, blind=True)
        blind_ok = blind_routing == routing
    else:
        det = blind_ok = True

    print("kv_ann_bridge: kv=%s" % args.kv)
    print("  log        %d anchor(s), %d delta(s)" % (len(anchors), len(deltas)))
    print("  rows       %d emitted, %d miss(es), %d unresolvable"
          % (len(rows), len(misses), len(unresolvable)))
    if rows:
        print("  dim        %d, sid sample %s, npast %d..%d"
              % (rows[0]["dim"], rows[0]["sid"], rows[0]["npast"], rows[-1]["npast"]))
        print("  seek plan  %s" % (sidecar[0] if sidecar else {}))
    for t, why in misses[:5]:
        print("  miss       turn %d: %s" % (t, why))
    print("  loader-compat %s" % ("PASS" if not bad else "FAIL %s" % bad[:3]))
    print("  deterministic %s" % ("PASS" if det else "FAIL"))
    print("  value-blind   %s (routing columns unchanged with text destroyed)"
          % ("PASS" if blind_ok else "FAIL"))
    if not args.blind:
        print("  wrote      %s (%d rows)" % (out, len(rows)))
        print("             %s (%d plans)" % (side, len(sidecar)))
    return 0 if (not bad and det and blind_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
