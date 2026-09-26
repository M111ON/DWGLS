# GOE Climate-Zone Filter — DRAFT (2026-09-26)

Status: **draft only, no implementation**. Light probe only. Taxonomy undecided.

## Idea (owner)

Visualization (`144×144` field) renders fast, never lags. Model weights are
equally static → reuse the same principle for **compute instead of visualize**.
Reuse the timeline (version-control axis) as a **climate-change field
structure**: not rewind, but **filter zone by data type**.

## Mapping to existing DWGLS (verified by reading code, not assumed)

| GOE claim | Existing counterpart | State |
|---|---|---|
| Field 20736 = `2⁸×3⁴` | sacred constants, `TESS_TOTAL_SLOTS` | proven |
| Bake once, serve many (USB) | `tess_gguf_pack` + `tesspack_stream_view` via `llama_model_init_from_user` | proven single-model |
| Activate = shift, not search | MoE top-K route, `anch_assign`+`anch_perm` (MOD-37), Peano entry/exit | proven |
| Fast views, no lag | one stored copy + pure-arithmetic views + mmap fault-on-demand | proven (render + breathe RSS) |
| Timeline as zone filter | `core/clim_record.h`: window `[k,k+20736)`, payload NEVER moves (RELABEL), `W` shared `[0,144)`, `router` = type/param pack (semantics **empty — this draft's open slot**) | mechanism proven, taxonomy open |
| Timeline as rewind | `geofs_mdim.h` journal + `state_at(F)` | proven, separate axis — do not conflate |

## Light probe (2026-09-26, `clim_record.h` only, zero core changes)

Probe: `clim_zone_probe.c` (temp dir, not in repo). **19/19 PASS**:

- `clim_init` bounds: `W=0` ok, `W=144` rejected (spec constant).
- Slide `+5000`: payload `memcmp` identical (bytes never move), `offset==5000`, sliding flag set, `verify==0`.
- `apply`/`invert` hand-checked: `slot0→5000`, `slot20735→25735`, outside positions `→ -1` (= filter decision, no rewind).
- Two zones, one payload: `router=1` window `[0,20736)`, `router=2` window `[20736,41472)` — membership filters correctly both directions (`pos100`∈A∉B, `pos20800`∈B∉A, slot `64` hand-computed), payload still untouched.
- Tamper: 1-bit flip `→ -2` (honest failure, no silent corruption).

Conclusion: the **mechanism** for climate-zone filtering already works with
checked-in code. What is missing is **meaning**, not machinery.

## Hard constraints (non-negotiable, from rules + header)

1. ONE shared scale `W` (`#6121`) — climate carries no second scale, ever.
2. MAP not COMPRESS — zone relations must be emergent (perm/route/color), never a stored per-node edge graph.
3. int-only, no float projection — any LSH/JL variant must be integer-deterministic.
4. Binary truth — any future ratio/filter-correctness claim needs decode-compare, never encode-only.

## Zone taxonomy: ZoneCard3 (owner decision 2026-09-26)

Use the existing 12B `ZoneCard3` (`core/zone_card_v3.h`) as the zone key —
no new taxonomy invented. Draft binding: `ClimRec.router` low-16 = card id.

| Card | Meaning | Gate verdict | Filter effect |
|---|---|---|---|
| GREEN / GOLD | verified healthy / canonical | FAST (1) | ADMIT fast path |
| BLUE | facts-backed | AUGMENT (2) | ADMIT + attach facts |
| WHITE | blank / writable | BLANK (3) | ADMIT fresh-write path |
| ordinary id | no special | 0 | ADMIT (no verdict) |
| RED / BLACK | breach / tomb | HALT (-1) | REJECT even in-window |
| WILDCARD (no dev) | hidden | HIDDEN (-3) | REJECT |
| WILDCARD (dev) | owner override | OVERRIDE (9) | ADMIT |
| any replay in job | spent | SPENT (-2) | REJECT |

Draft filter rule: `ADMIT(p) iff clim_invert==0 AND verdict is load-class`.
Single-use spent mask is the anti-spam mechanism (WILDCARD can't leak across jobs).

## Light probe 2 (2026-09-26, `zone_card_v3.h` + `clim_record.h`, zero core changes)

Probe: `zone_gate_probe.c` (temp dir, not in repo). **18/18 PASS**:

- Verdict table matches header switch verbatim (GREEN→1, BLUE→2, RED/BLACK→-1, WHITE→3, ordinary→0, replay→-2, WILDCARD→-3 hidden / 9 dev-override).
- Combo: `pos100` zoneA-GREEN→ADMIT, `pos100` zoneB→REJECT (window), `pos20800` zoneB-BLUE→ADMIT, BLUE replay→REJECT (spent), in-window RED→REJECT (halt), no-dev WILDCARD→REJECT (hidden).
- Payload `memcmp` identical after all gate play; both climate records still verify.

Conclusion: zone-card-as-taxonomy works mechanically with checked-in code.
Remaining (still open): `router` bit layout beyond low-16, zone↔Wang-color
mapping, anchor-bucket interplay, shared-field isolation proof, perf gates.

## Open decisions (taxonomy decided per owner — do not invent another)

- Zone key: model-id? weight-type (attn/mlp/moe)? semantic (plant/animal)? How many zones?
- `router` bit layout in `ClimRec` + interplay with Wang edge colors and anchor buckets.
- Shared-field isolation proof (zones must not collide — cf. `n%37==0` collapse class of bug).
- Compute-path perf gates (`#6149`: RSS under load, soak 1–4h, p99, corrupted+fuzz overhead).

## Recommended next step (approach A, still needs approval)

Router-only filter, 2 zones on real data, RELABEL only (fix = 32B record rewrite):
define `router` for 2 zones → serve-time filter → correctness = window-membership
agreement + payload `memcmp` → perf = RSS/p99 vs unfiltered baseline.
Approaches B (capo-partitioned zones) and C (shared-field + integer-LSH) stay
parked until A passes and taxonomy is decided.
