# HANDOFF — nomic embedding migration + ANN/tooling fixes (2026-11-14)

Session closed: the whole local retrieval stack moved from **multilingual-e5-small
(384-d)** to **nomic-embed-text-v2-moe (768-d)**, plus the audit/tooling repairs
that came out of it. This is the brief for the next session.

**Canonical standard + risk inventory + verify commands + rollback:**
`I:\tools\ann\docs\EMBEDDING-STANDARD-2026-11-14.md`
(it is also reachable from the global `AGENTS.md` trigger table, and the durable
facts are in project-memory `#8315`–`#8323`).

---

## 1. What is now true (verified)

| piece | state |
|---|---|
| one sidecar | `llama-server` b12032 + `F:\models\nomic-embed-text-v2-moe.f16.gguf`, `:18095`, `-c 2048 -b 2048 --ubatch-size 2048 -ngl 99` (~19 rows/s on the 1050 Ti) |
| one mount gate | `ann_keepalive.ensure_sidecar()` — health-check → held OS byte lock `.sidecar.lock` → re-check → spawn. Measured: **5 concurrent callers → 1 llama-server** |
| memcore store | `fact_store.v2.sqlite3` 54,373 / 54,373 rows at 768-d |
| ANN index | `data/memcore/ann_index.jsonl` rebuilt by `scripts/build_index.py`, `space_hash 0xa04897ae`, anchors **K=56**, roster 56 doors, stamp live 54,373 / unplaced 0 |
| query prefix | `ann_search.c` prefixes text queries with `LZ_EMBED_QUERY_PREFIX` (default `search_query: `) |
| magic-context | 2,697 memories + 838 git commits re-embedded at 768 under identity `embedding-provider:57ba1c77…`; config → `:18095`, `max_input_tokens 448` |
| pool link | chat-pool `:8390 /semantic` → ANN works (`ok:true`, top-1 correct); nothing to change |

**Measured quality** (same corpus, 27 queries): separation between gibberish and
real queries **0.259 with the prefix / 0.158 without / 0.021 for old e5**; the
correct document is top-1 only *with* the prefix. Cloud-vs-local cosine gate:
doc 0.9994, query 0.9995. Open risk: on a document-fragment test e5 scored higher
R@1 (0.36 vs 0.14, n=14, not significant) — long-document recall is unmeasured.

## 2. Commits

| repo | branch | commit | what |
|---|---|---|---|
| `I:\tools` | main | `710fad3` | ann: nomic stack, sidecar gate, query prefix, build_index space hash, rolling xray/jev log + `errors="replace"` |
| `I:\tools\cloud_ws` | main | `0d4ca75` | memcore: HTTP embedder to :18095, `search_document: ` prefix, embed_server fix |
| `I:\DWGLS-native-fs` | **feat/geo-native-fs** | `6857eb1` `4452417` `8652686` `9763a70` `7796ddb` `2405156` `85ec4a4` `0b53d19` | **pushed** to `origin/feat/geo-native-fs` |

`2405156` is a **cherry-pick of master's `9bcfc92`** (BCR + deterministic HNSW +
benches) — master was ahead of feat by exactly that one commit, so all of
`master`'s work now exists on `feat` too. The other curation commits put the new
core headers/tests/docs in their own groups; the two `tests/tmp_*.c` scratch
files and the stale `.cortexkit` config backup were removed, the two regenerable
raw `.txt` dumps were parked on F:, and the unrelated Ripple-65B local-API proxy
was moved to `experiments/local-api-ripple65b/`.

`85ec4a4` matters beyond bookkeeping: the project's own `.cortexkit/magic-context.jsonc`
still pinned `provider: local / Xenova/multilingual-e5-small / int8`, i.e. **this
project would have kept embedding with e5 in-process** while everything else used
the shared sidecar. That override is gone; the project inherits the global config.

**Tests that pass** (rerun them if you touch this area):
`test_b_fs_wire` 20/20 · `test_b_fs_multi` 12/12 · `test_b_fs_magnifier` 9/9 ·
`test_bcr` (BCR 141 cycles vs flat 21,016 = **149x**, lossless addr roundtrip) ·
`test_bfs_hnsw` (DHNSW 133,041 vs flat 791,046 = **5.95x**).

## 3. Next, ranked

1. **X-ray/JEV as a rerank/tuning experiment** (the reason they exist). Each audit
   row already carries `ann_score` + `xray.flags/pruned` + the JEV score, so the
   question "does pruning/reranking change recall@k" is answerable without
   touching the main service. Counting restarted from zero today (see §4).
2. **Decide whether `b_fs_wire` becomes the ANN storage behind `:8096`.**
   `core/b_fs_*` + `BfsSystem` pass their tests but have **no production
   consumer** — same shape as the HN-Jet case. Also decide whether the 26-cell /
   loshu / shifts / dim-bridge set is used by anything beyond its tests.
3. **Long-document recall** — the one place the migration may have regressed
   (fragment test favours e5). Needs a small labelled set before claiming the
   switch is a strict improvement.
4. **magic-context chunk lane** — ~3,135 rows still sit on the old 1024-d identity.
   They are inert (every reader filters `model_id = current`) and migrate only as
   their session is used; migrating them deliberately needs the plugin's chunk-text
   recipe (`chunk_hash` does not match a sha256 reconstruction).
5. **Not migrated on purpose** (separate spaces, re-embed only if you mean it):
   obsidian-memory (Ollama `bge-m3`), chatchunker → cloud_memory (Cloudflare
   `@cf/baai/bge-m3`), memcore `embed_store.py`/`query.py` (NVIDIA `nemotron-3-embed-1b`).
6. **`memory-daemon` is Disabled** — bulk embedding is pending→resume by design
   (`AGENT-RECIPES.md`: `--once --dry-run` shows the backlog, `--once` resumes,
   content-hash ids make a re-run free, `--idle 120` yields while opencode works).
   Run it in a quiet window if you want chat/pool rows back-filled.

## 4. Loose ends / warnings

- **X-ray/JEV pre-reset data is on F:** (`F:\nomic-migration-2026-11-14\xray-jev-pre-reset\`,
  xray 119 rows + jev 121 rows). It was un-usable for the new system (cell_id =
  index line number, and the index was rebuilt) — but keep it for comparison.
- **5 audit rows are unattributed**: after the reset the first few rows appeared
  faster than the dashboard's 15 s cadence, and their `ts_ms` is not in today's
  query log. Suspect a second writer (an old dashboard instance or another task).
  Watch whether the file grows 1 row / 15 s; if it double-grows, find the dupe.
- **The query log contains invalid UTF-8** (a byte sequence at ~5375 in
  `ann_query-20261114.jsonl`). Both auditors now read with `errors="replace"`,
  but the *writer* (`ann_search.c` qlog) still emits it — worth a look.
- **magic-context needs a restart** to pick up config changes (it has no config
  watcher). Its endpoint must be `http://127.0.0.1:18095/v1`.
- **Backups** (all on F:): `F:\nomic-reembed-2026-11-14\` (bundle + vectors +
  context.db pre/post), `F:\nomic-migration-2026-11-14\` (`ann-data-memcore\` 992 MB
  pre-switch, `fact_store.v2.pre-nomic.sqlite3`, `originals-2026-11-14\`,
  `pre-qprefix-2026-11-14\`, `xray-jev-pre-reset\`, `dwgls-regenerable\`).

## 5. Branch note

`I:\DWGLS` (master) and `I:\DWGLS-native-fs` (feat/geo-native-fs) are two
worktrees of one repo. Master is now **fully contained in feat** (its only unique
commit was cherry-picked as `2405156`), so `feat/geo-native-fs` is the single
place to continue; merging it back to master is a decision, not a need.
