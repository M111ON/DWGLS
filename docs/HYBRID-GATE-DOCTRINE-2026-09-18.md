# Hybrid Gate Doctrine — 2026-09-18 (night session)

Owner directive: hybrid composition — skill as skeleton/frame + truth-checking
personality + lightweight facts/semantic db + blast-radius method. Pool concept
extended 2D plane -> 3-4D geometric layer. Graft target: tools/MCP/plugin memory.

## 1. Measured (all on real runs, no synthetic claims)

| # | Finding | Evidence |
|---|---------|----------|
| 1a | Uncommon tokens disturbed ~1.3-1.6x more than common ones (2 models agree) | lora_delta_probe, Qwen2.5 rare .55/mid .63/com .44, Qwen3 .66/.69/.42 (#980) |
| 1b | Random rank-8 single-layer adapter derails free generation ~immediately | lora_diverge_probe: FIRST_DIVERGE 3/1, NDIFF 95-97/100 (#981) |
| 1c | Trained verify-habit adapter: talk up, accuracy NOT up | stage-1c paired 95: base 22/95 -> +adapter 17/95; fixed 6 / broken 11; verify-words 34->41 (#982) |
| 1d | First monotonic triple WITHDRAWN | artifact of transposed adapter layout; corrected via b10830 shape contract A=[ne0,r] B=[r,ne1] (#980) |
| 1e | Skeleton tripwire works on gross errors | 1+1=483 TRIPS, 1+1=2 passes; digit-count/mod9/bounds from question alone |

## 2. Literature (searched after 1c failed — it predicted us)

- Huang et al. 2023: intrinsic self-correction DECREASES accuracy; prior gains used oracle labels. We replicate.
- STaR/RAFT: SFT on correction traces raises fixes AND breaks, net negative. Our 6/11 matches exactly. Fix = stage-2 RL (+1 fix / -1 break).
- SCORE (ACL24): small LMs can refine but need a STRONG verifier to gate when-to-correct. = our stage-2 gate.
- Skill-to-LoRA 2026: behavior adapters want narrow targets (q,v), rank 16, teacher-distilled demos; skill-specific beats shared.
- "LoRA changes what you can teach": rank bottleneck learns surface style. Predicts our talk-up/accuracy-down.
- CoSC: external ground truth (code execution) as verifier. = our skeleton oracle.
(#983)

## 3. Components (built tonight, committed)

- `tools/lora_delta_probe.c` — per-token logit delta + freq buckets
- `tools/synth_lora_build.c` — GGUF v3 LoRA writer (any base model)
- `tools/lora_diverge_probe.c` — free-run cascade (FIRST_DIVERGE/NDIFF)
- `tools/lora_accuracy_probe.c` — paired accuracy eval harness
- `tools/make_verify_traces.py` — 100 fix/confirm traces (seeded)
- `tools/peft_to_gguf_lora.py` — PEFT safetensors -> GGUF LoRA (roundtrip-tested)
- `colab-pack/train_verify_lora.py` + `verify_traces.jsonl` + `verify_eval.jsonl` + `verify_lora.gguf` (trained rank-8, 16.8MB)
- `core/zone_card_v3.h` — 12B gate descriptor + 7 specials + single-use jobs + hidden WILDCARD
- graft/ built: 8004 nodes, 12308 edges, 802 cards (git-ignored local cache)

## 4. Rules (owner-confirmed, memories #984-#997)

1. Hybrid composition: skeleton (auditable text spec) + checking personality (adapter) + facts layer (memcore/pool) + blast radius. (#984)
2. Zone cards: 24 to start (ring, 288B, split while discriminable / merge when identical). id = coordinate, no lookup. (#985, #607)
3. 7 specials reserved 0xFFF8-0xFFFE: RED halt, BLUE facts-augment, GREEN fast, BLACK tomb, WHITE blank, GOLD canonical, WILDCARD god-hand. (#986)
4. Single-use per job (spent mask, replay = ALREADY_SPENT). WILDCARD hidden dev-only (HIDDEN without dev flag). (#986)
5. Domain check before value check: wrong-JOB (x12 answers 500) -> RED+reroute; wrong-SHAPE (1+1=483) -> retry. Every skill registers output domain in skeleton. (#651)
6. Retry-without-reroute is THE bug (Gemini evening). Retry budget + forced escalation, then HALT — never infinite re-roll. (#987)
7. Human "it is broken" = HALT signal, never a prompt for another artifact. (#988)
8. No proof = no GOLD. Unverified claims stay ordinary/WHITE. (#988)
9. Inner voice = machine channel (card fields -> gate), never user content. Raw voice dev/WILDCARD only. (#989)
10. Justification != verification. "Works" claims ship run receipts or stay unproven. = repo test-integrity (#105) extended to model behavior. (#990)
11. Edits carry scope cards (frozen outside scope, else RED). (#991)
12. Checking-personality speaks run receipts, never guarantees; admits seed-variance. (#991)
13. Mask = way to get along, not smart by default. Scoping enforced outside model (post-edit diff), never requested in prompt. (#992)
14. Cost: ordered gate (deterministic first), 12B cards = the cost optimization, per-check ledger, depth scales with stakes. (#994)
15. Onion growth: earned never pre-built; day-one cost ~zero; ledger plots effect-over-time; layers train later layers. (#996)
16. Crust relocation: MODEL-FREE transfers / MODEL-BOUND re-derives; store raw always; tag model id. (#997)
17. Watch en route for anything solving intrinsic scoping fundamentally. (#993)
18. Scale: cage economics improve with size; containment/capability scale independently; no claim on 100B+ without a run. (#995)

## 5. Roadmap

- [ ] D1 gate prototype: calibrate v3 thresholds from CSV distributions; 24 cards over 500 positions; gate picks adapter zone (offline, no GPU)
- [ ] D2 blast-radius proof: route-scoped eval (adapter only on math zone; non-math bit-identical vs broadcast 95% derail)
- [ ] D3 adapter v2 (Colab): teacher demos filtered by gold, narrow targets q/v rank 16, separate verifier, DPO second stage
- [ ] D4 gate ledger persistent (trace+verdict+cost per job, cross-session) + split/merge rules + domain-first graft routing (card -> skeleton/ask shortlist)
- [ ] D5 full composition wired to MCP/plugin memory graft network

## 6. Open (explicitly unproven)

- Novelty-gating at 100B+ scale. Adapter v2 accuracy gain. Gate ledger schema. Card split/merge thresholds.
  Rule: no claim without a run (rule 10).
