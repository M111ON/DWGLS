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

## Candidate fix in `I:\tools\ann` (not applied)

Accept `index_size + K + anchors_cksum` as sufficient for `unchanged` when the
mtime differs but the content fingerprint matches; the checksum already proves the
anchors are the ones this index produced. Cost of the change: one comparison.
Benefit: a restored or copied index stops paying a 59 s kmeans re-derive for a
timestamp. Left unimplemented because it changes another repo's contract.
