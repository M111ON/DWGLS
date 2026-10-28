#!/usr/bin/env python3
"""kv_turns_from_pool.py — real chat-pool messages as KV temporal prompts.

The producer has always run on a built-in seed script or a hand-made prompt
file; neither is traffic. This pulls N consecutive messages of ONE app out of a
pool log (append-only, so file order is the thread's own order) and writes two
files:

  <out>          one prompt per line, plain text (what kv_temporal_llama reads)
  <out>.src.jsonl {turn, line, app, kind, hash, chars, sha256}

`hash` is the pool line's own hash: that is the identity the ANN side already
uses for a record, so the bridge can join a frame back to the exact pool line
instead of matching text.

Read-only. Point it at a copy unless you mean to read the live log (it never
writes to the log either way).
"""
import argparse
import json
import os
import re
import sys
import unicodedata

# kinds that carry a sentence worth carrying as context
DEFAULT_KINDS = ("CHAT", "chat", "DECISION", "HANDOFF", "msg", "bridge", "block")


def norm(t):
    """Same normalization the bridge joins on: NFKC + collapsed whitespace."""
    t = unicodedata.normalize("NFKC", t)
    return re.sub(r"\s+", " ", t).strip()

def strip_envelope(t):
    """Drop the pool record's own bookkeeping, keep the message.

    A pool line's text carries `[tag#id] (user|ai n/m) <ISO timestamp>` in front
    of the sentence. Embedded whole, that prefix dominates every vector: measured
    on 16 consecutive real messages, adjacency recall@1 was 0.125 (chance) with
    the envelope and 0.500 without, @3 0.688 -> 0.875. The envelope is identity,
    not content, and the identity travels as the record hash instead.
    """
    t = re.sub(r"^\[[^\]]*\]\s*", "", t)
    t = re.sub(r"\((?:user|ai[^)]*)\)\s*", "", t)
    t = re.sub(r"\d{4}-\d\d-\d\dT[\d:.]+(?:[+-]\d\d:\d\d)?\s*", "", t)
    return t.strip()


def scan(path, kinds, skip_apps, min_len, max_chars, keep_envelope):
    out = []
    with open(path, "rb") as f:
        for idx, raw in enumerate(f, 1):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if not line.endswith("}"):
                continue
            try:
                d = json.loads(line)
            except Exception:
                continue
            if d.get("kind") not in kinds:
                continue
            app = d.get("app") or ""
            if app in skip_apps:
                continue
            record = norm(d.get("text") or "")
            body = norm(strip_envelope(record))
            # selection is always made on the record, so a stripped run and a
            # whole-record run can be compared over exactly the same messages
            if len(record) < min_len:
                continue
            text = record if keep_envelope else body
            if len(text) > max_chars:
                text = text[:max_chars].rstrip()
            out.append({"line": idx, "app": app, "kind": d.get("kind"),
                        "hash": d.get("hash") or "", "t": d.get("t"),
                        "text": text, "body": body[:max_chars].rstrip(),
                        "record_chars": len(record)})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=int, default=12)
    ap.add_argument("--app", default=None, help="default: the app with the most messages")
    ap.add_argument("--kinds", default=",".join(DEFAULT_KINDS))
    ap.add_argument("--skip-app", default="kv-temporal")
    ap.add_argument("--min-len", type=int, default=40)
    ap.add_argument("--max-chars", type=int, default=400,
                    help="keep one turn inside the model's context budget")
    ap.add_argument("--keep-envelope", action="store_true",
                    help="write the [tag] (role) timestamp prefix too (measured worse: "
                         "adjacency recall@1 0.125 vs 0.500)")
    ap.add_argument("--also-stripped", default=None,
                    help="also write the envelope-stripped bodies of the SAME selection, "
                         "so two runs differ only in what was embedded")
    ap.add_argument("--start", type=int, default=0,
                    help="skip this many qualifying messages of the chosen app first")
    ap.add_argument("--turn-base", type=int, default=0,
                    help="offset for the recorded turn numbers (append batches: pass the "
                         "number of turns already in the log)")
    args = ap.parse_args()

    kinds = tuple(k.strip() for k in args.kinds.split(",") if k.strip())
    skip_apps = tuple(a.strip() for a in args.skip_app.split(",") if a.strip())
    rows = scan(args.log, kinds, skip_apps, args.min_len, args.max_chars,
                keep_envelope=args.keep_envelope)

    counts = {}
    for r in rows:
        counts[r["app"]] = counts.get(r["app"], 0) + 1
    app = args.app or (max(counts.items(), key=lambda kv: kv[1])[0] if counts else None)
    if app is None:
        print("no qualifying message in %s" % args.log, file=sys.stderr)
        return 1

    picked = [r for r in rows if r["app"] == app][args.start:]
    if len(picked) < args.limit:
        print("app %s has %d message(s) after --start %d, need %d"
              % (app, len(picked), args.start, args.limit), file=sys.stderr)
        return 2
    picked = picked[:args.limit]

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for r in picked:
            f.write(r["text"] + "\n")
    if args.also_stripped:
        with open(args.also_stripped, "w", encoding="utf-8", newline="\n") as f:
            for r in picked:
                f.write(r["body"] + "\n")
    with open(args.out + ".src.jsonl", "w", encoding="utf-8", newline="\n") as f:
        for i, r in enumerate(picked, 1):
            f.write(json.dumps({"turn": args.turn_base + i, "line": r["line"],
                                "app": r["app"], "kind": r["kind"], "hash": r["hash"],
                                "chars": len(r["text"])}, ensure_ascii=False) + "\n")

    print("kv_turns_from_pool: %s" % args.log)
    print("  chosen app %s (%d qualifying message(s) in the log)" % (app, counts[app]))
    print("  wrote      %d prompt(s) -> %s" % (len(picked), args.out))
    print("  provenance %s.src.jsonl (pool hash per turn)" % args.out)
    print("  lines      %s" % [r["line"] for r in picked][:12])
    print("  chars      min %d max %d" % (min(len(r["text"]) for r in picked),
                                          max(len(r["text"]) for r in picked)))
    print("  sample     %s" % picked[0]["text"][:100])
    return 0


if __name__ == "__main__":
    sys.exit(main())
