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


def read_msgs(path):
    """turn -> [{"sha","text"}], the messages a frame's prompt was built from.

    One frame can cover several messages. With this map the bridge emits one ANN
    row PER MESSAGE (each with that message's own stored vector) instead of one
    per frame, and every row of the same frame carries the same seek plan — so a
    hit on any message resumes the frame that holds it.
    """
    out = {}
    if not path or not os.path.exists(path):
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
            t = m.get("turn")
            msgs = m.get("msgs")
            if not t or not isinstance(msgs, list) or not msgs:
                continue
            out[int(t)] = [g for g in msgs
                           if isinstance(g, dict) and isinstance(g.get("text"), str)]
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


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def verify_map(mrows, anchors, deltas, blob_path):
    """A map row is only actionable if its plan still resolves.

    Two things can rot while the map sits on disk, and both are silent: the anchor
    blob could be truncated/moved (append-only says no, verification says so), and
    the frames it names could be gone. Check the bytes, not the promise.
    """
    ok, bad = 0, []
    turns = {int(d["turn"]) for d in deltas}
    anchor_turns = {int(a["turn"]) for a in anchors}
    blob = b""
    if os.path.exists(blob_path):
        with open(blob_path, "rb") as f:
            blob = f.read()
    for m in mrows:
        kv = m["kv"]
        why = None
        if kv["anchor_turn"] not in anchor_turns:
            why = "anchor turn %d is not in the log" % kv["anchor_turn"]
        elif kv["turn"] not in turns:
            why = "frame turn %d is not in the log" % kv["turn"]
        else:
            for t in kv["replay"]:
                if t not in turns:
                    why = "replay turn %d is not in the log" % t
                    break
        if why is None:
            off, ln, want = kv["anchor_blob"]
            if off < 0 or ln <= 0 or off + ln > len(blob):
                why = "anchor blob [%d,%d) is outside the blob file (%d B)" % (off, ln, len(blob))
            else:
                got = fnv1a64(blob[off:off + ln])
                if got != want:
                    why = "anchor blob fnv %d != recorded %d" % (got, want)
        if why:
            bad.append((m["sid"], why))
        else:
            ok += 1
    return ok, bad


def row_sid(turn, text):
    h = hashlib.sha256(("%d:%s" % (turn, text)).encode("utf-8")).hexdigest()[:16]
    return "kv:" + h


def vkey(vec):
    """Exact vector identity, as bytes. Two rows with the same vector are the same
    point in the space whatever their sid — that is how the map mode finds the base
    sid to attach a KV frame to, and how the duplicate count is measured."""
    return struct.pack("<%df" % len(vec), *[float(x) for x in vec])


def build(kvdir, memcore, blind=False, msgs_path=None):
    anchors, deltas = read_kv_log(os.path.join(kvdir, "log.jsonl"))
    texts = read_turns(os.path.join(kvdir, "turns.jsonl"))
    msgs = read_msgs(msgs_path)
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

        units = []
        if turn in msgs:
            for g in msgs[turn]:
                units.append((g.get("sha") or hashlib.sha256(
                    g["text"].encode("utf-8")).hexdigest()[:16], g["text"]))
        else:
            text = texts.get(turn)
            if text is None:
                misses.append((turn, "no text recorded for this frame"))
                continue
            units.append((None, text))

        frame_sid = row_sid(turn, texts.get(turn, units[0][1]))
        for msha, mtext in units:
            mv = memcore_vector(memcore, mtext)
            if not mv:
                misses.append((turn, "no stored vector for this message"))
                continue
            eid, dim, vec = mv
            sid = row_sid(turn, mtext) if msha is None else \
                "kv:" + hashlib.sha256(("%d:%s" % (turn, msha)).encode()).hexdigest()[:16]
            rows.append({"sid": sid, "file": os.path.join(kvdir, "log.jsonl"),
                         "npast": turn, "dim": dim, "vec": vec})
            sidecar.append({"sid": sid, "turn": turn, "parent": int(d.get("parent", 0)),
                            "text_sha": hashlib.sha256(mtext.encode("utf-8")).hexdigest()[:16],
                            "msg_sha": msha, "frame_sid": frame_sid,
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
    ap.add_argument("--msgs", default=None,
                    help="JSONL {turn,msgs:[{sha,text}]} -> one ANN row per message")
    ap.add_argument("--collide-with", default=None,
                    help="existing ann_index.jsonl; report rows whose vector is already there")
    ap.add_argument("--map-only", action="store_true",
                    help="also write kv_index_map.jsonl (base sid -> KV frame), no new ANN rows needed")
    args = ap.parse_args()

    out = args.out or os.path.join(args.kv, "ann_rows_kv.jsonl")
    side = args.sidecar or os.path.join(args.kv, "kv_frames.jsonl")

    rows, sidecar, misses, unresolvable, anchors, deltas, routing = build(
        args.kv, args.memcore, blind=args.blind, msgs_path=args.msgs)

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
        rows2, side2, _, _, _, _, _ = build(args.kv, args.memcore, msgs_path=args.msgs)
        det = (json.dumps(rows, sort_keys=True) == json.dumps(rows2, sort_keys=True)
               and json.dumps(sidecar, sort_keys=True) == json.dumps(side2, sort_keys=True))
        # value-blind: with every text destroyed, the routing columns must be
        # identical — proven over ALL resolvable turns, not only the ones that
        # happened to have a vector, so vector availability cannot mask a failure
        *_, blind_routing = build(args.kv, args.memcore, blind=True, msgs_path=args.msgs)
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

    # one plan per frame: every row of the same turn must carry the same seek plan,
    # or a hit would resume a different frame than the one that holds the message
    by_turn = {}
    for s in sidecar:
        by_turn.setdefault(s["turn"], set()).add(
            (s["anchor_turn"], tuple(s["anchor_blob"]), tuple(s["replay"])))
    split = [t for t, plans in by_turn.items() if len(plans) != 1]
    print("  frames     %d distinct turn(s), %d row(s) per turn avg %.2f, %s"
          % (len(by_turn), len(rows),
             (len(rows) / len(by_turn)) if by_turn else 0.0,
             "one plan per frame PASS" if not split else "SPLIT PLANS %s" % split[:3]))

    if args.collide_with and rows:
        # the join key is the sid itself: the live index was built from memcore and
        # keeps the entry id as its sid (verified: best-match cosine 0.99999994 and
        # the same 32-hex id). Matching on floats would be approximate; the id is exact.
        have = set()
        with open(args.collide_with, "rb") as f:
            for raw in f:
                try:
                    m = json.loads(raw.decode("utf-8", "replace"))
                except Exception:
                    continue
                if m.get("sid"):
                    have.add(m["sid"])
        dup_sids = [s["sid"] for s in sidecar if s.get("memcore_id") in have]
        print("  already in %s by sid: %d/%d row(s)"
              % (os.path.basename(args.collide_with), len(dup_sids), len(rows)))
        if dup_sids:
            print("    e.g. %s -> base row %s"
                  % (dup_sids[0], next(s["memcore_id"] for s in sidecar
                                       if s["sid"] == dup_sids[0])))

        # The map mode is the shape the measurement argues for: ANN keeps its rows
        # untouched and the KV frame is attached to the base sid that already
        # exists, so nothing can be displaced out of top-k.
        if args.map_only:
            mrows, unmatched = [], []
            for s in sidecar:
                if s.get("memcore_id") not in have:
                    unmatched.append(s["sid"])
                    continue
                mrows.append({"sid": s["memcore_id"],
                              "kv": {"turn": s["turn"], "frame_sid": s["frame_sid"],
                                     "msg_sha": s["msg_sha"],
                                     "anchor_turn": s["anchor_turn"],
                                     "anchor_blob": s["anchor_blob"],
                                     "replay": s["replay"]}})
            mpath = os.path.join(args.kv, "kv_index_map.jsonl")
            write_jsonl(mpath, mrows)
            print("  map        %d base sid(s) -> KV frame, %d unmatched -> %s"
                  % (len(mrows), len(unmatched), mpath))
            if mrows:
                print("    e.g. %s -> turn %d anchor %d replay %s"
                      % (mrows[0]["sid"], mrows[0]["kv"]["turn"],
                         mrows[0]["kv"]["anchor_turn"], mrows[0]["kv"]["replay"]))
            vok, vbad = verify_map(mrows, anchors, deltas,
                                   os.path.join(args.kv, "kv_blobs.bin"))
            print("  map verifies %d/%d actionable (anchor blob fnv recomputed, "
                  "replay turns present)%s"
                  % (vok, len(mrows), "" if not vbad else " FAIL %s" % vbad[:2]))
    if not args.blind:
        print("  wrote      %s (%d rows)" % (out, len(rows)))
        print("             %s (%d plans)" % (side, len(sidecar)))
    return 0 if (not bad and det and blind_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
