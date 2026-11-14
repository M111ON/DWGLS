# AGENTS.md — DWGLS (4Dimension Geometry + KIS Timeline) · router (L0)

<!-- MEMANTO-MANAGED-SECTION -->
<!-- memanto-template-version: 1.1.0 -->
## MEMANTO - Long-term memory

This project uses Memanto for long-term memory across sessions. You MUST actively evaluate whether to use the `memanto` MCP tools (`recall`, `remember`) or CLI commands (identifying yourself with `--source mt2`) at every step.

### 1. WHEN TO RECALL (Search Context)
- **TASK INITIATION**: Starting a new feature, evaluating PR reviews, reviewing external code, or planning a refactor.
- **AMBIGUOUS REPAIR**: Facing a cryptic build failure, test failure, or environment bug.
- **UNSTATED PREFERENCE**: Choosing a library, pattern, or naming convention not explicitly specified.
- **EXPLICIT USER QUESTION**: The user asks to check memory, recall context, or asks "what did we decide?"

### 2. WHEN TO REMEMBER (Store Context)
- **PREFERENCE / INSTRUCTION**: The user states a code formatting style, library choice, or tool preference.
- **GOAL / COMMITMENT**: A target feature, roadmap deliverable, or sprint objective is established.
- **LEARNING / ERROR**: You extract a bug fix workaround, error post-mortem, or test failure insight.
- **DECISION / OBSERVATION**: You make an architectural decision, or record an API quirk / technical constraint.
- **FACT**: A key OS behavior, path layout, or tool version requirement is verified.

### 3. THE ABSTRACTION RULE (How to think about memory)
When you store a memory, ELEVATE IT TO A PRINCIPLE.
- **WRONG (Activity Log)**: "User told me to use functional components."
- **RIGHT (Universal Principle)**: "Exclusively use functional components for React UI."
Do not record the conversation. Record the universal rule.

### 4. THE DURABILITY TEST (What NOT to store)
Ask yourself: *"Will this generalized principle fundamentally change how I generate code for this user 3 months from now?"*
- **DO NOT STORE**: Step-by-step progress, routine bug fixes, UI tweaks, temporary code snippets, or literal chat summaries.

Just use the tools or commands; there's no need to mention memory checks in your text replies.

<!-- /MEMANTO-MANAGED-SECTION -->

<!-- MEMANTO-DYNAMIC-MEMORIES -->
<!-- /MEMANTO-DYNAMIC-MEMORIES -->

## Step 0 — self-detect (do this FIRST, before anything else)
1. Estimate your usable context. If you are a **small/local model** (roughly ≤ 20k, or you were told to run light): load ONLY `.agents/chatmode.md` next, then stop loading and follow it.
2. ถ้า detect ว่าให้ pickup handoff, ทำงานต่อจากที่ค้างไว้ให้รัน full mode ได้เลย
3. Otherwise (normal cloud model): continue below.

## Mandatory (all apps, all sizes — no exceptions)
- Read `.agents/rules.md` IN FULL before doing any work. Prohibitions there override everything else.

## Tools 
-suggest try using "graft grep <query>" instead of grep and "graft ask <query>" 
**more info just type "graft" in terminal 
-check drive and process before run a high usage task, kill unused/duplicate if needed with carefully checked.
-alway set timeout or spawn separate console when running task that stuck inside (idle, loop)

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

##suggestion

## User Requested answered Pattern
- User specialize on pattern matching and comparing structure, alway explain the result in 2 version contains:
 - technical universal terms (honestly, ไม่เดา, มีหลักฐานรองรับ, ถ้าไม่มีให้แจ้ง, ตรงไปตรงมา)
 - casual less technical summarizes (comparison if possible) ขอคำอธิบายแบบบ้านง่ายๆ