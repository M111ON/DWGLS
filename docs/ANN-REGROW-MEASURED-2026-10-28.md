# regrow: 4 ms, 115 ms, or 59 s — which one you get depends on the state sidecar

2026-10-28 · `tools/kv_regrow_probe.py` · measured against `I:\tools\ann` HEAD
`245d27c`, binary rebuilt from source before the run (`build/ann_search.exe`,
2026-10-28 12:58; the shipped exe was from 2026-10-26 23:37 and predated
`f6f7afc`/`be03cec`).

Earlier in this workstream I reported "full regrow 82 s at 63,915 rows" and used
it as a cost of keeping a second index. That number was real but it was the
**wrong mode** — it was a full re-derive on a scratch copy, not the steady-state
path. Corrected below.

## The three modes, as implemented

`regrow_state_path()` = `ANN_ANCH_PATH` with `.bin` → `-state.json`, holding
`{n_prev, last_sid, index_mtime, index_size, K, anchors_cksum}`.

| mode | condition | measured |
|---|---|---|
| **unchanged** | state matches index mtime+size+K+anchors cksum | **4.2 / 5.3 / 13.4 ms**, `skipped:1` |
| **incremental** | index grew by append, line `n_prev-1` still carries `last_sid` | **115.1 ms** for +32 rows on 63,915 |
| **full** | state missing/corrupt/contract failed, or `force:1` | **56.3–59.2 s** at 63,915 rows |

Receipts (`I:\Temp\annfull\kv_regrow_probe.json`, `I:\Temp\regrow_run.txt`):

```
full sidecars, nothing appended, copied index   → anchor/full         59,185 ms  n_prev 0    n_new 63,915
full sidecars, +32 rows appended                → anchor/incremental     115 ms  n_prev 63,915 n_new 32
partial sidecars (no .stamp), +32 rows appended → anchor/full         56,276 ms  n_prev 0    n_new 63,947
settled work dir, regrow ×3                     → anchor/unchanged   13.4 / 4.2 / 5.3 ms  skipped 1
```

## Why my copy fell into the full path

`kv_regrow_probe.py` copies with `shutil.copyfile`, which writes the content but
**not the mtime**. The unchanged fast path keys on
`index_mtime + index_size + K + anchors_cksum`; size, K and checksum all matched —
only the mtime differed — so the contract failed and the server re-derived all
63,915 anchors with kmeans. The same thing happens on any restore/rsync that does
not preserve timestamps, and there is **no branch for "identical content, new
mtime"**: with nothing appended, `incremental` does not apply either (it requires
`n > n_prev`), so the only path left is `full`.

That is the operational rule this experiment produces:

> copy the index together with its sidecars **and** its mtime, or accept a ~59 s
> re-derive. A missing `.stamp` / `-state.json` / `tag_lut.json` forces the same.

## Consequence for the earlier conclusions

* "A second KV index costs an 82 s regrow" — **wrong**. Steady state is 4–13 ms
  (nothing changed) or 115 ms (append). The 59 s figure applies only to a first
  build or to a copy that broke the state contract.
* The split verdict in `docs/KV-INDEX-SPLIT-2026-10-28.md` stands on its retrieval
  numbers, and its cost line is corrected there.
* `kv_index_split_probe.py` and `kv_ann_live_probe.py` both copy only part of the
  sidecar set, so their regrow calls were full re-derives by construction. Their
  retrieval measurements are unaffected (routing was derived before querying), but
  no regrow timing from those two tools should be quoted.

## The fix, applied and verified (2026-10-28, `I:\tools\ann` 86c93af)

The gate keyed on the mtime, and mtime is not content. The state sidecar now
carries `index_fp` = FNV1a-64 over `(size, n_prev, anchors_cksum, first 4 KB,
last 4 KB)`; when the mtime differs the unchanged path compares that
fingerprint instead of falling through to a full re-derive. A state written
before the change gets one in place, but only while the mtime still proves the
file is the one it describes.

`tools/ann_regrow_upgrade_probe.py` runs each case on its own copy of the live
63,915-row index, booted and queried:

| case | mode | ms | verdict |
|---|---|---|---|
| A copy, mtime dropped | unchanged | 4.7 | PASS (the fix) |
| D copy, mtime kept | unchanged | 3.9 | PASS (old contract) |
| B copy, mtime dropped, +32 rows | incremental | 177.5 | PASS |
| C copy, last sid edited | full | 56,291.9 | PASS (refused) |
| U old state upgraded in place | unchanged | 5.6 | PASS (`index_fp` written) |
| E copy, **first** sid edited | full | 57,196.8 | PASS (refused) |

`make test` ALL PASS. Live `:8096` restarted on this build: `/v1/state/regrow`
→ `mode unchanged, ms 3.6`, the state file now carries
`index_fp:2569272408841868000`, and a self-query returns top-1 at 1.000000 with
real neighbours at 0.9982/0.9981.

**What the fingerprint does not cover**: an edit in the middle of the 263 MB
index. Only a full-file hash would see that (~0.1-0.2 s at NVMe speed, still
inside the 1 s budget if it is ever needed). The two windows cover
append/truncate (size), a retrain (anchors_cksum), and edits to the first or
last row.

**Why a tail-line check alone was not enough** (the first version of this fix):
it proved the last row only, so a first-row edit passed as unchanged and the
server would route by stamps that no longer describe the row. Case E is that
test; it fails the tail-only version and passes the fingerprint.
