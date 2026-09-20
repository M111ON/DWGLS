# AGENTS.md — DWGLS (4Dimension Geometry + KIS Timeline) · router (L0)

## Step 0 — self-detect (do this FIRST, before anything else)
1. Estimate your usable context. If you are a **small/local model** (roughly ≤ 32k, or you were told to run light): load ONLY `.agents/chatmode.md` next, then stop loading and follow it.
2. Otherwise (normal cloud model): continue below.

## Mandatory (all apps, all sizes — no exceptions)
- Read `.agents/rules.md` IN FULL before doing any work. Prohibitions there override everything else.

## Tools 
-suggest try using "graft grep <query>" instead of grep and "graft ask <query>" 
**more info just type "graft" in terminal 

## Tier map (load on demand, shallow → deep)
| Tier | File | When |
|------|------|------|
| L1 | `.agents/rules.md` | always (mandatory) |
| L2-shared | `.agents/project.md` | architecture, state, tools, build |
| L2-app | `.agents/opencode.md` | ONLY if you are OpenCode |
| L3 | `docs/`, `graft/INDEX.md`, git log | deep dives, one file at a time |

## App routing (don't hunt for tools that aren't yours)
- **OpenCode** → read `.agents/opencode.md` (graft, memory MCPs, handoff, local cmds).
- **Any other app** → SKIP the L2-app file entirely. If a doc references a tool you don't have (a `*.cmd`, an MCP name, a CLI), do NOT go searching the system for it — ask the user or work without it.

## Shared core (so every app agrees)
- MAP not COMPRESS — coordinate = address. 20736 = one cell. ★ 6ico/144 protagonist.
- Working dirs: `I:\DWGLS-native-fs`, `I:\model`, `F:\model`, `I:\llama`. English/Thai only.
