# Sync Report 2026-09-16 — OpenCode Marathon Data Recovery

## สถานะปัจจุบัน

| Component | Location | Size | สถานะ |
|---|---|---|---|
| **OpenCode CLI** | `C:\nvm4w\nodejs\opencode.cmd` | — | v1.18.31 ✅ |
| **Desktop app** | `AppData\Local\@opencode-aidesktop\` | 2.9 GB | AppData (Electron, normal) |
| **Session DB** | `I:\Opencode\opencode.db` → `I:\OpenCode-data\` | 1.2 GB | symlink ✅ (recovered 367 sessions from backup) |
| **Local DB** | `I:\OpenCode-data\opencode-local.db` | 0.3 MB | 1 session (fresh install, Sep 3 only) |
| **Vault exports** | `I:\.vault\exported_md\` | 395 files | Sep 14 batch export ✅ |
| **fact_store** | `I:\tools\cloud_ws\memcore\fact_store.sqlite3` | 122 MB | 7,624 entries, 591 DWGLS ✅ |
| **memory.db** | `I:\tools\cloud_ws\memory.db` | 0 bytes | Empty (cloud-memory not wired) |
| **pool.db** | `I:\tools\cloud-workspace\pool.db` | 0.1 MB | 4 sessions, 9 chunks |
| **C:\ disk** | System drive | 111 GB (6.8 GB free) | **94% — freed 4.1 GB from Temp cleanup** |

## Data Flow: OpenCode Sessions → Memory Stores

```
OpenCode Desktop (Sep 12-14 marathon, 39+ commits)
    ↓ DB was reset on reinstall
    ↓ Only 367 sessions recovered (up to Sep 5) from .bak
    ↓ Sep 12-14 sessions NOT in recovered DB
    ↓
    ├→ I:\.vault\exported_md\ (395 session MDs, exported Sep 14)
    │   → fact_store ingestion (7,624 entries, 256 from vault exports)
    │
    ├→ magic-context (708 memories, 473 compartments)
    │   → last updated Aug 14 (zen investigation)
    │   → NO Sep 12-15 data
    │
    └→ git history (41 commits Sep 12-14)
        → CODE IS TRUTH — 144/146 TIER1 + 4/4 TIER2 PASS
```

## OpenCode Sep 12-14 Marathon: 41 Commits

### Phase 1: Geometry Primitives (Sep 12)
| Commit | Feature | Tests |
|--------|---------|-------|
| `6de3472` | Hex-Quad-Dual upgrades #1-6 + Makefile | GEO_SLOW 36/36 |
| `0626891` | Hex-Quad-Dual verification report | docs |
| `3063f61` | Audit response — dead branch, entropy UB, iso_rot90→iso_hex5 | GEO_FAST |
| `9858aa6` | Fractal geometry + twin rebalance + GPU pipeline | 135 assertions |

### Phase 2: Planet v2 Lifecycle (Sep 12-13)
| Commit | Feature | Tests |
|--------|---------|-------|
| `e994b69` | 12-pentagon face-spawn registry (PlanetSys) | 8/8, GEO_FAST 29/29 |
| `4049756` | Planet v2 RDH-blueprint (u64 keys, origin tomb) | 13/13 |
| `c56bc20` | Planet on real GGUF + real FGXLog replay | 6/6 |
| `b43b9c3` | Planet detach frame + idle-zero tail + tombstone | 11/11, GEO_FAST 25/25 |
| `3f41d53` | Planet watches every BFS block (birth at write) | 5/5 |
| `e52668d` | Entangle self-gating link (auto-open/close) | 15/15+9/9+7/7 |
| `cb3e02e` | Planet contraction journal (hyperbolic-side log) | 17/17 |

### Phase 3: BFS Operations (Sep 13)
| Commit | Feature | Tests |
|--------|---------|-------|
| `c2fe90b` | BFS delete retire-then-free + tomb archive | 4/4 |
| `e1770a9` | BFS migrate wrap-relocate (surgery primitive) | 4/4 |
| `d55b504` | Explicit fold operator (compact on demand) | 4/4 |
| `6e9ad36` | v4 image (tomb tail + rebirth, v3 tolerant) | 5/5 |

### Phase 4: Dual Loop + Field Views (Sep 13)
| Commit | Feature | Tests |
|--------|---------|-------|
| `25d5fdf` | Dual loop 20→12 merge + 12→20 split (idempotent settle) | 6/6 |
| `2ca791c` | 32-unit field view (20+12 summed, 8x81 dual rail) | 7/7 |
| `e5b92fd` | RDH twin (high-nibble second fuse, 64x81) | 9/9 |

### Phase 5: Topology + Deployment (Sep 13-14)
| Commit | Feature | Tests |
|--------|---------|-------|
| `ed5965c` | Goldberg(4,0) frame 162x128 + pentagon map | 8/8, GEO_FAST 26/26 |
| `33137b4` | GP(4,0) neighbor topology (subdivide+dual) | 6/6 |
| `d7f938b` | Goldberg neighbor topology (generated 162x6 table) | 13/13 |
| `7cac800` | Inner field digit-extension (approach A) | 9/9, GEO_FAST 24/24 |
| `133811d` | Wiring queue: D4 triality + L-block + RR gate + voronoi fix | — |
| `195b1fa` | A2×A2 symmetry + D4 triality bridge + bake▶inference | — |
| `937a9d0` | ARM Termux deploy pipeline + report | 24/27 compile |

## Key Decisions (from sessions)
1. **Planet = BFS observer** — watches every block, auto-gates via entanglement
2. **RDH-blueprint** — u64 keys with origin tomb, fail-closed thaw
3. **Dual loop** — 20→12 merge (5→1 XOR) + 12→20 split, idempotent settle
4. **Goldberg GP(4,0)** — neighbor topology for 162×6 addressing
5. **Inner field digit-extension** — approach A (address-only nesting)
6. **iso_rot90 → iso_hex5** — renamed from order-5 to order-6 hexagonal symmetry
7. **ARM Termux** — 24/27 compile on SM-G988B, 3 failures are clang-specific

## Disk Cleanup Done
| Action | Space Freed |
|--------|------------|
| Deleted Temp .tmp files (tesspack_server, assembled GGUF) | ~4.1 GB |
| **C:\ before** | 2.7 GB free (98%) |
| **C:\ after** | 6.8 GB free (94%) |

## Remaining Issues
1. **OpenCode DB Sep 12-14 sessions lost** — DB reset during reinstall, backup only goes to Sep 5. Vault exports (395 MDs) contain the session transcripts but aren't indexed by OpenCode.
2. **memory.db empty** — cloud-memory DB at `I:\tools\cloud_ws\memory.db` has 0 bytes. Needs initialization or wiring.
3. **C:\ at 94%** — Desktop app (2.9 GB) + nvm (1.7 GB) + Docker (1.7 GB) + Chrome (4.4 GB) are the top hovers.
4. **2 test failures** — `test_safetensors_reader`, `test_cap_tune_safetensors` (pre-existing, need model files)
5. **magic-context stale** — Last update Aug 14. OpenCode sessions from Sep 12-14 not yet synced.

## DB Symlink Status
```
I:\Opencode\opencode.db      → I:\OpenCode-data\opencode.db       (1.2 GB) ✅
I:\Opencode\opencode-local.db → I:\OpenCode-data\opencode-local.db  (0.3 MB) ✅
I:\Opencode\opencode.db.md5   → I:\OpenCode-data\opencode.db.md5    (symlink) ✅
```

## Vault Session Analysis (5 largest sessions, from .vault/exported_md/)

### Inner Field L0/L1/L2 (`core/geo_inner_field.h`)
- L0 = existing (tess, cube, slot), L1 = 144³ via digit extension, L2 = 20736³ = 12¹²
- Choice A: `(q,l)` → `(q·16+q', l·9+l')` — matches th_node hierarchy
- 9/9 tests pass, mutation 16→15 red 4 pts, GEO_FAST 24/24
- L1/L2 is naming scheme (view of L0), NOT new storage layer

### Planet Detach + Entangle (full lifecycle)
- Detach: own frame, snapshot before/after 5000 ticks → bit-identical
- Tail (FGXLog): idle-zero, error = `{W, expected, observed}`, cap 256
- Born at max size, can only contract — ceiling-load rule
- Tombstone: 24B plate `{id, birth_W, death_W, final_home, digest}`
- Auto-reanchor: tail full → adopt current bytes as baseline (rc=2)
- Real GGUF test: planet on 4MB Qwen2.5-0.5B-Q8_0, 2000 ticks, 26 reanchors, FGXLog real data

### Goldberg Frame GP(4,0) (`core/geo_goldberg_frame.h`)
- 162 faces × 128 slots = 20736 (sacred)
- Pentagons 0–11 (uniform), hexagons 12–161
- Euler: V=482, E=1440, F=960 → 482-1440+960=2 ✓

### KIS v6b → breathing_fs Integration (`core/bfs_v6b_adapter.h`)
- DynContainer (`dwgls_dynamic_codec.h`) deprecated → moved to deprecated/
- v6b streaming API: 7.6× faster roundtrip than v4
- block_encoded buffer 512→2048 (v6b worst-case Q8 residual 868 bytes)
- Performance: v4=3.8ms/2.03×, v6=0.5ms/1.14×, v6b=0.6ms/2.01×

### Bugs Fixed (11 total)
1. IF_L2_MAX → IF_L2_SLOTS (off-by-one)
2. TESS_CELLS macro collision (144 vs 8 → 1152)
3. v6b emit buffer too small (hardcoded 4+144*6)
4. v6b streaming emit loop (must loop until 0)
5. V6B_DC_MAX_ENC too small (1024→2048)
6. Anchor delta scale mismatch (T22)
7. Block count expectations wrong (T8, T35)
8. Hand-written pairing table geometrically impossible
9. K162_eq-edge specs wrong (1442→482V)
10. MinGW stack overflow (2MB→16MB)
11. v5 angular grid collision (abandoned, documented)

### `.opencode-mem` Data Audit
| Source | Sessions | Date Range | Sep 12-14? |
|--------|----------|------------|------------|
| opencode.db (recovered) | 367 | Aug 30 – Sep 5 | ❌ |
| .opencode-mem (live + File History) | 188 | Jun 18 – Jul 3 | ❌ |
| vault exported MDs | 395 files | Sep 14 export | ✅ |
| fact_store.sqlite3 | 7,624 entries | mixed | ✅ 591 DWGLS |
| magic-context | 708 memories | Aug 13-14 | ❌ |

**结论**: `.opencode-mem` หยุดเขียนตั้งแต่ Jul 3 (ไม่ได้ migrate ข้าม install). Sep 12-14 marathon data อยู่ใน vault exports + git history เท่านั้น

## Recommendations
1. **Ingest vault sessions → fact_store** — Run `build_store.py` in memcore to index the 395 vault MDs
2. **Initialize memory.db** — Run cloud-memory MCP init
3. **Monitor C:\ disk** — Docker (1.7GB) + Chrome (4.4GB) are top hovers
4. **Post-checkout hook verified** — auto-restores file dates
5. **OpenCode `.opencode-mem` stops updating** — investigate why it froze at Jul 3; may need manual trigger or config fix
