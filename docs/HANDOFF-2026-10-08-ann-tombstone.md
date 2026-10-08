# Handoff — ANN + Tombstone (2026-10-08, pickup for next session)

Full report lives in **`docs/ANN-SYSTEM-REPORT-2026-10-08.md`** — read that first.
This file is the pickup sheet: what state the box is in, what is proven, what is
next, and the traps.

---

## 0. TL;DR for the next session

- ANN search **works** on real data: live serve on 8096 + embed on 18095, index
  = 2881 chunks / 121 docs. Deterministic, correct.
- **Tombstone persistence** landed (`820ec9e`): fog survives restart, proven live.
- **Nothing is uncommitted.** Working tree is clean; HEAD = `1c84a1f`.
- The next real lever is the **tombstone warm path** (wire route order to read the
  latch), then **measure warm vs cold recall**. Do not raise `ANCHR_MAXK` expecting
  pruning — measured, it does not help.

---

## 1. State of the box (verify before trusting)

```bash
cd I:/DWGLS-native-fs
git log --oneline -1        # expect: 1c84a1f chore: fix Tri_semi gitignore path...
git status --porcelain      # expect: empty
wc -l build/kv_index.jsonl  # expect: 2881
```

Servers (may have died if the chat model was switched — restart per §5):
- embed sidecar on **18095** (`multilingual-e5-small`, 384-dim)
- lazy serve on **8096**

Known flake (NOT a real failure): `test_pair_table` opens 4 real GGUF files and
occasionally reports `RUN FAIL` inside a full `make test` run. It passes 20/20
standalone and 3/3 full-suite re-runs. If it flakes, re-run before investigating.

---

## 2. What is proven [measured]

| thing | evidence | commit |
|---|---|---|
| Live ANN search | 6 live queries → 200, correct docs, scores 0.85–0.88 | — |
| Tombstone survives restart | cold `replayed=0/shut=6` → kill → warm `replayed=25/shut=7` | `820ec9e` |
| Live-HTTP test | hermetic 16 PASS+1 SKIP; live 22 PASS/0 FAIL | `6574183` |
| Capacity curve | scan linear in N (1.38ms@2881 → 56ms@92192), ~1.48MB/1k @dim384 | `a71b78b` |
| SIFT1M recall | maze b128 recall@10 0.9617 @ 5.32% scan, index 0.44MB | docs |

---

## 3. The brake points (do not re-derive — they are measured)

- **B1 hard-bucket wall** — exact recall costs 100% of the index. Better centroids
  do not fix it (k-means fit 0.934→0.944 moves cost 100%→~60% only).
- **B2 `LZ_ANCH_MAXK = 8`** (`tools/gguf_lazy_serve.c:273`) — routing is capped at
  8 buckets while N grows. `ANCHR_MAXK` in the header is 512; the serve clamps to 8.
- **B3 budget auto-raises to N** (`:471`) — the walk is a full scan today.
- **B4 per-bucket cap is the wrong accelerator** — cap 512 → recall 0.6236,
  cap 2048 → 0.6501. Cutting members cuts recall faster than it buys speed.
- **B6 maze loses HNSW latency 84×** but wins index memory ~680×. We compete on
  memory + build, not query speed.

Full detail + reproduce steps: `docs/ANN-SYSTEM-REPORT-2026-10-08.md` §4.

---

## 4. Next work (ordered — pick up here)

1. **Wire the tombstone warm path.** Route order should read the latch: shut
   branches first, OPEN after, with a budget stop. Then **measure warm vs cold
   recall separately** to prove the tombstone does not touch novel recall.
   - Touch: `tools/gguf_lazy_serve.c` `lz_mem_search` (the `rnb`/`rb` route block,
     around line 493–504), `core/geo_wang_latch.h` (`wl_is_open` is already there).
   - Gate: a warm (repeat) query must score fewer items than a cold one, with the
     same top-k.
2. **Decide the operating point** — exact-no-prune vs prune-approximate
   (topb=4 → recall 0.875 at 62% scan). Product choice, not a code change.
3. **Wire the refine layer** — `tools/maze_walk_cli.c` (kNN graph, greedy descent)
   as a second stage after the anchor route. The only path that attacks latency.
4. **Prove or drop the field bridge** — `experiments/field-bridge-2026-10-08/`.
5. **P3 route-log compaction** (CLEAR epoch) so the log does not grow unbounded.

---

## 5. How to run it (exact commands)

```bash
# embed sidecar (must cd into its dir — the exe is a launcher needing sibling DLLs)
cd I:/llama/llama-b10830-win-vulkan-x64
./llama-server.exe -m I:/model/multilingual-e5-small-q8_0-fixed.gguf \
    --embedding --port 18095 -c 512 &

# serve (from repo root; needs the CPU backend dir to avoid Vulkan false FAIL)
cd I:/DWGLS-native-fs
LZ_EMBED_HOST=127.0.0.1 LZ_EMBED_PORT=18095 LZ_ROUTE_LOG=build/lz_route.log \
PATH="I:/llama/llama-v040-bin-win-vulkan-x64:$PATH" \
./build/gguf_lazy_serve.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf \
    --serve 8096 build/backend_cpu &

# query
curl -s -X POST -H "Content-Type: application/json" \
  -d '{"query":"tesspack format spec","topk":3}' \
  http://127.0.0.1:8096/v1/state/search
```

Build the serve (from `make lazy-serve` or manually — needs llama DLLs + `-lws2_32`):
```bash
gcc -O2 -std=c11 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-format \
  -I core -I I:/llama/include -o build/gguf_lazy_serve tools/gguf_lazy_serve.c \
  I:/llama/llama-v040-bin-win-vulkan-x64/{llama,ggml,ggml-base,ggml-cpu-x64}.dll \
  -lzstd -lpsapi -lws2_32 -lm
```

**Kill the serve correctly** (MSYS mangles `/F`; use the env prefix):
```bash
MSYS_NO_PATHCONV=1 taskkill /F /PID <pid>
```

---

## 6. Traps (learned the hard way)

- **`LZ_ROUTE_LOG` must be a native Windows path.** `/tmp/...` silently fails —
  the exe cannot create it and the tombstone never persists. Use `build/lz_route.log`.
- **The patch tool injects CRLF churn** on this repo's Makefile. After any
  `patch` on the Makefile, `git diff` and check the line count; fix byte-level in
  Python if the diff is not the one line you meant.
- **`write_file` refuses** on files it has not fully read this task. Use
  `execute_code` with direct `open(...,'w')` when rewriting a whole file.
- **`make test` compiles to `build/test-<name>`** (double `test-` prefix); the
  binary name in the group list is the source stem, not the binary.
- **Verify the binary after every gcc** on this box — pipes can mask link failures.
- **Parked ≠ wired.** `experiments/field-bridge-2026-10-08/` and
  `experiments/ann-capacity-2026-10-08/` compile standalone but are deliberately
  NOT in `make test` and have no Makefile target. Keep it that way until asked.

---

## 7. Parked concepts (context, not tasks)

- **Pomato / Hourglass** — `experiments/apex-hidden-2026-09-29/`. Hourglass =
  lossless fold (64→16→4→1, signed gates) with a mirror twin; a correct/verify
  primitive, not search. Pomato = the two outputs of one stem, tombstone = stem.
- **Field bridge** `12×6×8×6×6 = 20736 = 12⁴` — storage ladder and tesseract
  ladder are the same product. **No cube-assembly code exists and none is
  required**; `6 == 6` is a rule identity, not assembly. Observation only.

---

## 8. Commits from this session

```
1c84a1f chore: fix Tri_semi gitignore path, AGENTS output style, cortexkit schedules
cd51c07 colab-pack: ANN bench driver (modes 0-5, NQ x reps, siftsmall)
49bf7b9 ann_pipe_real: fuse the valve into the pipe entrance (all modes one byte pass)
1065ce7 anchor_route: raise ANCHR_MAXK 64 -> 512 (bucket-space headroom)
6574183 test: opt-in live-HTTP coverage for the search path (+ -lws2_32)
a71b78b docs: ANN system report + park field-bridge / capacity concepts
820ec9e #62 tombstone index: fingerprint appended to P3 log, survives restart
```

`make test` = **154/154 + TIER2 4/4**, exit 0, from committed HEAD.
