# DWGLS Makefile — 4D Geometry + KIS Timeline
# ═══════════════════════════════════════════════
# Usage:
#   make test          — compile + run all tier-1 tests
#   make test-TIER1    — same as above
#   make test-NAME     — compile + run single test
#   make clean         — remove build artifacts
#   make list          — list available tests

CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -I. -Icore -Icore/infra -Icore/infra -no-pie
LDFLAGS := -lm

# ── Auto-discover test sources ────────────────────────
ALL_TESTS := $(patsubst tests/%.c,%,$(wildcard tests/*.c))

# ── Tier 1: self-contained (no missing deps) ──────────
TIER1 := \
  geo_cube_in_dodeca_test \
  kis_4d_explore \
  kis_alternating_verify \
  kis_codec_v6_standalone_test \
  kis_adaptive_deploy \
  kis_container_place \
  section4_seal_residual \
  test_cell_classify \
  test_cube_addr \
  test_cube_container \
  test_cube_in_dodeca \
  test_geo_diamond_map \
  kis_birds_eye \
  kis_multi_container \
  kis_scale_test \
  test_geo_prune \
  test_geo_fs \
  test_geo_fs_mdim \
  test_geo_fs_generalize \
  test_monitor \
  test_phi_microscope \
  test_safetensors_reader \
  test_tess_index_frame \
  test_tess_scale_log \
  test_tess_frame_seek \
  test_tess_scale_dedup \
  test_tess_magnify \
  test_tess_hex_delta \
  test_tess_sacred \
  test_dodeca_x2 \
  test_walk_sync \
  test_walk_bench \
  test_parity_sector \
  test_cache_locality \
  test_tess_subdivide \
  test_tess_scale_wire \
  test_tess_tetra_axis \
  test_tess_torus \
  test_tess_tetra_torus \
  test_tess_12x1728 \
  test_geo_sync_bridge \
  test_tess_geo_jump_walks \
  test_tess_full_cycle \
  test_tess_belt \
  test_tess_tensor_belt \
  test_tess_ghost \
  test_tess_leverage \
  test_tess_registry_gate \
  test_tess_trace \
  test_tess_wiring \
  test_v5_collision \
  test_gguf_box \
  test_gguf_window_chain \
  test_gguf_real_gate \
  test_gguf_multi_model \
  test_bfs_persist \
  test_bfs_stability \
  test_geo_bfs_hub \
  test_bfs_seek_anchor \
  test_bfs_breath \
  twin_seeker_test \
  twin_seeker_hard_test \
  test_6ico_tesseract \
  test_hyper_delta_format \
  test_residual_space \
  test_ghost_lift \
  test_ghost_envelope \
  test_cap_account \
  test_cap_tune_real \
  test_cap_tune_safetensors \
  test_cap_tune_fs \
  test_cap_chain_roundtrip \
  test_cap_chain_big \
  test_rs_persist \
  test_cap_scheme \
  test_breathing_fs \
  test_shell \
  test_tess_header \
  test_geofs \
  test_tess_codec \
  test_fibo_checkpoint \
  test_fibo_walk \
  test_fibo_dual_rail \
  test_geo_hyperbolic \
  test_geo_hyper_fs \
  test_geo_hyper_real \
  test_rdh_addr \
  test_iso_rot90 \
  test_tied_dedup \
  test_goldberg_decagram \
  test_goldberg_store \
  test_goldberg_file \
  test_goldberg_lazy \
  test_ggf_walk \
  test_goldberg_mmap \
  test_ggf_walk_mmap \
  test_ggf_ckpt_replay \
  test_ggf_fs \
  test_geo_dual_view \
  test_geo_lblock \
  test_wang_tantrix \
  test_hyp_fusion \
  test_ghost_direct \
  test_pair_table \
  test_hybrid_kv \
  test_kv_remap \
  test_kv_remap_diamond \
  test_kv_geofs_bridge \
  test_kv_rail_geofs \
  test_kv_dramtile \

# ── เทสต์ที่เหลือ (ไม่ได้อยู่ใน TIER1/TIER2) = legacy ประวัติการพัฒนา ──
# เขียนก่อน rescope 2026-08-14 — เก็บไว้ย้อนดูเท่านั้น ไม่ใช้ยืนยันระบบปัจจุบัน
# รายละเอียด: docs/LEGACY_TESTS.md

# ── Tier 2: need gguf_reader.h or geo_frame_seek.h ────
# (removed: kis_codec_v5_test, kis_codec_v6_test, kis_map_roundtrip,
#  kis_real_gguf_test, test_geo_sid_verify, test_rail_hub,
#  test_qwen3_microscope, test_real_gguf_microscope, test_geo_inference,
#  test_geo_sid_loader — cross-repo deps, hang on fail)
TIER2 := \
  kis_codec_v4_test \
  test_geo_tensor_hub \
  test_geo_zerocopy \
  test_geo_rail_hub

# ── Tier 2 include dir (gguf_reader.h moved here) ────
TIER2_CFLAGS := $(CFLAGS) -Icore

# ── Build targets ─────────────────────────────────────
BUILD := build

.PHONY: all test clean list tier1 tier2 help

all: test

# ── Single test ───────────────────────────────────────
test-%: tests/%.c | $(BUILD)
	@echo "▶ BUILD  $<"
	$(CC) $(CFLAGS) -o $(BUILD)/$@ $< $(LDFLAGS)
	@echo "▶ RUN    $@"
	@./$(BUILD)/$@ && echo "✅ $@ PASS" || echo "❌ $@ FAIL"

# ── Run all tier-1 tests ──────────────────────────────
test: tier1 tier2
	@echo "═══════════════════════════════════════"
	@echo "TIER1: $(words $(TIER1)) tests"
	@echo "TIER2: $(words $(TIER2)) tests"
	@echo "═══════════════════════════════════════"

tier1: | $(BUILD)
	@pass=0; fail=0; \
	for t in $(TIER1); do \
	  if $(CC) $(CFLAGS) -o $(BUILD)/test-$$t tests/$$t.c $(LDFLAGS) 2>/dev/null; then \
	    if ./$(BUILD)/test-$$t >/dev/null 2>&1; then \
	      echo "  ✅ $$t"; pass=$$((pass+1)); \
	    else \
	      echo "  ❌ $$t (RUN FAIL)"; fail=$$((fail+1)); \
	    fi; \
	  else \
	    echo "  ❌ $$t (BUILD FAIL)"; fail=$$((fail+1)); \
	  fi; \
	done; \
	echo "───────────────────────────────────────"; \
	echo "PASS: $$pass / $$((pass+fail))  FAIL: $$fail"

tier2: | $(BUILD)
	@pass=0; fail=0; \
	for t in $(TIER2); do \
	  if $(CC) $(TIER2_CFLAGS) -o $(BUILD)/test-$$t tests/$$t.c $(LDFLAGS) 2>/dev/null; then \
	    if ./$(BUILD)/test-$$t >/dev/null 2>&1; then \
	      echo "  ✅ $$t"; pass=$$((pass+1)); \
	    else \
	      echo "  ❌ $$t (RUN FAIL)"; fail=$$((fail+1)); \
	    fi; \
	  else \
	    echo "  ❌ $$t (BUILD FAIL)"; fail=$$((fail+1)); \
	  fi; \
	done; \
	echo "───────────────────────────────────────"; \
	echo "TIER2 — PASS: $$pass / $$((pass+fail))  FAIL: $$fail"

# ── CLI binaries ─────────────────────────────────────
MDIM_CLI_BIN := $(BUILD)/mdim_cli

mdim_cli: tools/mdim_cli.c core/geofs_mdim.h | $(BUILD)
	@echo "▶ BUILD  mdim_cli"
	$(CC) $(CFLAGS) -o $(MDIM_CLI_BIN) tools/mdim_cli.c $(LDFLAGS)
	@echo "✅ mdim_cli ready → ./$(MDIM_CLI_BIN) help"

bench_mdim_timeline: tools/bench_mdim_timeline.c core/geofs_mdim.h | $(BUILD)
	@echo "▶ BUILD  bench_mdim_timeline"
	$(CC) $(CFLAGS) -o $(BUILD)/bench_mdim_timeline tools/bench_mdim_timeline.c $(LDFLAGS)
	@echo "✅ bench ready → ./$(BUILD)/bench_mdim_timeline"

# ── Streaming chain scan (whole folder — manual, not TIER1) ──
cap_scan: tools/cap_chain_scan.c core/geo_cap_account.h core/geo_ghost_lift.h | $(BUILD)
	@echo "▶ BUILD  cap_chain_scan"
	$(CC) $(CFLAGS) -o $(BUILD)/cap_chain_scan tools/cap_chain_scan.c $(LDFLAGS)
	@echo "✅ cap_scan ready → ./$(BUILD)/cap_chain_scan [root] (default F:/notebookLM)"

# ── Pair-table lazy refresh cost (write-heavy stream — manual) ──
pair_scan: tools/pair_refresh_scan.c core/geo_cap_account.h core/geo_ghost_lift.h | $(BUILD)
	@echo "▶ BUILD  pair_refresh_scan"
	$(CC) $(CFLAGS) -o $(BUILD)/pair_refresh_scan tools/pair_refresh_scan.c $(LDFLAGS)
	@echo "✅ pair_scan ready → ./$(BUILD)/pair_refresh_scan [root] [batch] [max_files]"

# ── Adaptive scheme decision (per-file vs global — manual) ──
cap_scheme: tools/cap_scheme_choose.c core/geo_placement_choose.h | $(BUILD)
	@echo "▶ BUILD  cap_scheme_choose"
	$(CC) $(CFLAGS) -o $(BUILD)/cap_scheme_choose tools/cap_scheme_choose.c $(LDFLAGS)
	@echo "✅ cap_scheme ready → ./$(BUILD)/cap_scheme_choose [root]"

# ── Silk-screen feasibility (unique maps under rotation+reversal — manual) ──
silk_scan: tools/silk_screen_scan.c core/gguf_box.h | $(BUILD)
	@echo "▶ BUILD  silk_screen_scan"
	$(CC) $(CFLAGS) -o $(BUILD)/silk_screen_scan tools/silk_screen_scan.c $(LDFLAGS)
	@echo "✅ silk_scan ready → ./$(BUILD)/silk_screen_scan [model.gguf ...]"

# ── Two-gap fill probe (deterministic transform + residual — manual) ──
two_gap_fill: tools/two_gap_fill.c | $(BUILD)
	@echo "▶ BUILD  two_gap_fill"
	$(CC) $(CFLAGS) -o $(BUILD)/two_gap_fill tools/two_gap_fill.c $(LDFLAGS)
	@echo "✅ two_gap_fill ready → ./$(BUILD)/two_gap_fill [wav]"

# ── T1.1 normal/bump/displacement map probe (manual) ──
normal_map: tools/normal_map_probe.c core/gguf_reader.h | $(BUILD)
	@echo "▶ BUILD  normal_map_probe"
	$(CC) $(CFLAGS) -o $(BUILD)/normal_map_probe tools/normal_map_probe.c $(LDFLAGS)
	@echo "✅ normal_map ready → ./$(BUILD)/normal_map_probe --syn smooth|sine2d|noise <n> | --file <path> <cols> | --gguf <model> <tensor_idx>"

# ── Hosoya fibo grid × geo_seed 12-coset (SVG decode + coupling — manual) ──
hosoya_seed: tools/hosoya_seed_probe.c core/geo_seed.h | $(BUILD)
	@echo "▶ BUILD  hosoya_seed_probe"
	$(CC) $(CFLAGS) -o $(BUILD)/hosoya_seed_probe tools/hosoya_seed_probe.c $(LDFLAGS)
	@echo "✅ hosoya_seed ready → ./$(BUILD)/hosoya_seed_probe"

# ── b-bond: direct resolve vs scan — chunk ไม่ต้องรันเลข (manual) ──
bond_direct: tools/bond_direct_resolve.c | $(BUILD)
	@echo "▶ BUILD  bond_direct_resolve"
	$(CC) $(CFLAGS) -o $(BUILD)/bond_direct_resolve tools/bond_direct_resolve.c $(LDFLAGS)
	@echo "✅ bond_direct ready → ./$(BUILD)/bond_direct_resolve"

# ── Bond จาก tetris: a[1]b[2]b[3]a — external/private semantics (manual) ──
bond_tetris: tools/bond_tetris_probe.c | $(BUILD)
	@echo "▶ BUILD  bond_tetris_probe"
	$(CC) $(CFLAGS) -o $(BUILD)/bond_tetris_probe tools/bond_tetris_probe.c $(LDFLAGS)
	@echo "✅ bond_tetris ready → ./$(BUILD)/bond_tetris_probe"

# ── Candidate → hyperbolic role map (20736 = 3 axes × 2 cylinders — manual) ──
hyp_candidate_map: tools/hyp_candidate_map.c | $(BUILD)
	@echo "▶ BUILD  hyp_candidate_map"
	$(CC) $(CFLAGS) -o $(BUILD)/hyp_candidate_map tools/hyp_candidate_map.c $(LDFLAGS)
	@echo "✅ hyp_candidate_map ready → ./$(BUILD)/hyp_candidate_map"

# ── Cube-look vs Cylinder-manage probe (20736 = 18×8×144 = 6×3456 — manual) ──
cube_cylinder: tools/cube_cylinder_probe.c | $(BUILD)
	@echo "▶ BUILD  cube_cylinder_probe"
	$(CC) $(CFLAGS) -o $(BUILD)/cube_cylinder_probe tools/cube_cylinder_probe.c $(LDFLAGS)
	@echo "✅ cube_cylinder ready → ./$(BUILD)/cube_cylinder_probe"

# ── RDH vs FNV-1a micro-benchmark (rdtsc cycle-accurate — manual) ──
rdh_bench: tools/rdh_bench.c core/geo_rdh_addr.h core/gguf_box.h | $(BUILD)
	@echo "▶ BUILD  rdh_bench"
	$(CC) $(CFLAGS) -o $(BUILD)/rdh_bench tools/rdh_bench.c $(LDFLAGS)
	@echo "✅ rdh_bench ready → ./$(BUILD)/rdh_bench [model.gguf ...]"

# ── Fibo clock checkpoint-replay sweep (custom table/field/dist/volume/pattern) ──
# --sweep เขียน image+manifest ลง build/ckpt + verify จากดิสก์ใน fresh process ทุก config
# --verify-all [DIR] ตรวจใหม่ทีหลัง · --economy X.X ปรับ threshold verdict
fibo_sweep: tools/fibo_checkpoint_sweep.c core/geo_ghost_lift.h core/infra/fibo_spine.h core/fibo_walk.h | $(BUILD)
	@echo "▶ BUILD  fibo_checkpoint_sweep"
	$(CC) $(CFLAGS) -o $(BUILD)/fibo_checkpoint_sweep tools/fibo_checkpoint_sweep.c $(LDFLAGS)
	@echo "✅ fibo_sweep ready → ./$(BUILD)/fibo_checkpoint_sweep [--sweep|key=value ...|--verify-all|--verify-img=IMG,CFG]"

# ── 3-way speed comparison: MAP(geometric) vs Classic file I/O vs RAM floor ──
# MAP  = dram_addr -> mmap offset (coordinate = address, no hash)
# Classic = contiguous file + index (traditional FS floor)
# RAM  = heap memcpy (physical ceiling)
geo_bench: tools/geo_speed_bench.c core/infra/geo_dram_tile.h | $(BUILD)
	@echo "▶ BUILD  geo_speed_bench"
	$(CC) $(CFLAGS) -o $(BUILD)/geo_speed_bench tools/geo_speed_bench.c $(LDFLAGS)
	@echo "✅ geo_speed_bench ready → ./$(BUILD)/geo_speed_bench"

geo_kv_bench: tools/geo_kv_real_bench.c core/gguf_reader.h core/infra/geo_dram_tile.h | $(BUILD)
	@echo "▶ BUILD  geo_kv_real_bench"
	$(CC) $(CFLAGS) -o $(BUILD)/geo_kv_real_bench tools/geo_kv_real_bench.c $(LDFLAGS)
	@echo "✅ geo_kv_real_bench ready → ./$(BUILD)/geo_kv_real_bench <model.gguf>"

gguf_hybrid_bench: tools/gguf_hybrid_bench.c core/gguf_reader.h | $(BUILD)
	@echo "▶ BUILD  gguf_hybrid_bench"
	$(CC) $(CFLAGS) -o $(BUILD)/gguf_hybrid_bench tools/gguf_hybrid_bench.c $(LDFLAGS)
	@echo "✅ gguf_hybrid_bench ready → ./$(BUILD)/gguf_hybrid_bench <model.gguf>"

# ── Housekeeping ──────────────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

# ── Batch converter (cross-repo: DWGLS + FGLS_new) ────
BATCH_SRC := tools/geo_batch_convert.c
BATCH_BIN := $(BUILD)/geo_batch_convert.exe
BATCH_CFLAGS := $(CFLAGS) -I I:/FGLS_new/runner

batch: $(BATCH_SRC) | $(BUILD)
	@echo "▶ BUILD  batch converter"
	$(CC) $(BATCH_CFLAGS) -o $(BATCH_BIN) $(BATCH_SRC) $(LDFLAGS)
	@echo "✅ geo_batch_convert.exe ready"

# ── Run batch: make batch-run GGUF=I:/model/file.gguf DIR=./output
batch-run: batch
	@$(BATCH_BIN) $(GGUF) $(DIR)

list:
	@echo "All tests ($(words $(ALL_TESTS))):"
	@for t in $(ALL_TESTS); do echo "  $$t"; done
	@echo ""
	@echo "Tier 1 (self-contained): $(words $(TIER1))"
	@echo "Tier 2 (need gguf_reader): $(words $(TIER2))"

help:
	@echo "make test       — compile + run tier-1 tests"
	@echo "make test-NAME  — run single test (e.g. make test-test_cell_classify)"
	@echo "make tier2      — list blocked tests"
	@echo "make vis        — start FGLS_vis visualizer (port 5001)"
	@echo "make vis GGUF=I:/model/model.gguf — visualizer with model"
	@echo "make clean      — remove build/"
	@echo "make list       — list all tests"

# ── GeoFS MDIM CLI ───────────────────────────────────────
mdim: $(BUILD)/mdim_cli

$(BUILD)/mdim_cli: tools/mdim_cli.c core/geofs_mdim.h | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/mdim_cli tools/mdim_cli.c $(LDFLAGS)
	@echo "✅ mdim_cli ready — create/summon/get/list/info/view/history/unsummon"

# ── Tier 3: llama.cpp graft (step ③) — needs I:/llama + Qwen GGUF ──
LLAMA_INC = I:/llama/include
LLAMA_DLL = I:/llama/llama-b9733-bin-win-vulkan-x64
LLAMA_GGUF ?= I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf

# Cactus graft: assemble graft GGUF from gguf_box (header scion + zero-copy
# body from the source mmap), load it with real llama.cpp, compare inference
# logits bitwise with the original file, and prove reroute-link routing.
graft-llama:
	@test -f $(LLAMA_DLL)/llama.dll || { echo "  (skip: llama DLLs not found — needs I:/llama/llama-b9733-bin-win-vulkan-x64)"; exit 0; }
	@test -f $(LLAMA_GGUF) || { echo "  (skip: $(LLAMA_GGUF) not found)"; exit 0; }
	@mkdir -p build
	$(CC) -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-macro-redefined -Wno-format \
	    -I core -I $(LLAMA_INC) -o build/test_gguf_graft_llama tests/test_gguf_graft_llama.c \
	    $(LLAMA_DLL)/llama.dll $(LLAMA_DLL)/ggml.dll $(LLAMA_DLL)/ggml-base.dll \
	    $(LLAMA_DLL)/ggml-cpu-x64.dll -lzstd -lm
	PATH="$(LLAMA_DLL):$$PATH" ./build/test_gguf_graft_llama $(LLAMA_GGUF) $(LLAMA_DLL)

# ── Real generation through the graft (multi-token, greedy) ──
# Uses the same llama.cpp DLLs as graft-llama; skips if they are absent.
graft-gen: | $(BUILD)
	@test -f $(LLAMA_DLL)/llama.dll || { echo "  (skip: llama DLLs not found — needs I:/llama/llama-b9733-bin-win-vulkan-x64)"; exit 0; }
	@test -f $(LLAMA_GGUF) || { echo "  (skip: $(LLAMA_GGUF) not found)"; exit 0; }
	$(CC) -O2 -std=c11 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-macro-redefined -Wno-format \
	    -I core -I $(LLAMA_INC) -o $(BUILD)/gguf_graft_generate tools/gguf_graft_generate.c \
	    $(LLAMA_DLL)/llama.dll $(LLAMA_DLL)/ggml.dll $(LLAMA_DLL)/ggml-base.dll \
	    $(LLAMA_DLL)/ggml-cpu-x64.dll -lzstd -lm
	PATH="$(LLAMA_DLL):$$PATH" ./$(BUILD)/gguf_graft_generate $(LLAMA_GGUF) "The capital of France is" 40

# ── Field-baked graft: body FROM the KIS field (not the mmap) ──
# Writes tensor bytes into the window chain, rebuilds a GGUF from the field,
# and proves llama.cpp generation is bitwise identical to the original.
graft-field: | $(BUILD)
	@test -f $(LLAMA_DLL)/llama.dll || { echo "  (skip: llama DLLs not found — needs I:/llama/llama-b9733-bin-win-vulkan-x64)"; exit 0; }
	@test -f $(LLAMA_GGUF) || { echo "  (skip: $(LLAMA_GGUF) not found)"; exit 0; }
	$(CC) -O2 -std=c11 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-macro-redefined -Wno-format \
	    -I core -I $(LLAMA_INC) -o $(BUILD)/gguf_graft_field tools/gguf_graft_field.c \
	    $(LLAMA_DLL)/llama.dll $(LLAMA_DLL)/ggml.dll $(LLAMA_DLL)/ggml-base.dll \
	    $(LLAMA_DLL)/ggml-cpu-x64.dll -lzstd -lm
	PATH="$(LLAMA_DLL):$$PATH" ./$(BUILD)/gguf_graft_field $(LLAMA_GGUF) "The capital of France is" 40

# ── Hybrid store graft: big → contiguous field · tiny → DtSlotRegion ──
# Wires the hybrid layout (BENCH Workload 3/4) into the graft path: tiny
# tensors go through size-classed DtSlotRegion (direct address), big through
# the contiguous window chain; rebuilds a GGUF and proves generation bitwise.
graft-hybrid: | $(BUILD)
	@test -f $(LLAMA_DLL)/llama.dll || { echo "  (skip: llama DLLs not found — needs I:/llama/llama-b9733-bin-win-vulkan-x64)"; exit 0; }
	@test -f $(LLAMA_GGUF) || { echo "  (skip: $(LLAMA_GGUF) not found)"; exit 0; }
	$(CC) -O2 -std=c11 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-macro-redefined -Wno-format \
	    -I core -I $(LLAMA_INC) -o $(BUILD)/gguf_graft_hybrid tools/gguf_graft_hybrid.c \
	    $(LLAMA_DLL)/llama.dll $(LLAMA_DLL)/ggml.dll $(LLAMA_DLL)/ggml-base.dll \
	    $(LLAMA_DLL)/ggml-cpu-x64.dll -lzstd -lm
	PATH="$(LLAMA_DLL):$$PATH" ./$(BUILD)/gguf_graft_hybrid $(LLAMA_GGUF) "The capital of France is" 40

# ── Output on the +37 belt: model's token+logits stream → field ──
# Step ⑤: real generation through the field-built graft, then embed the
# captured output (tokens + full per-step logits) into the field in +37 belt
# serial order; read back and prove bitwise identity.
graft-belt: | $(BUILD)
	@test -f $(LLAMA_DLL)/llama.dll || { echo "  (skip: llama DLLs not found — needs I:/llama/llama-b9733-bin-win-vulkan-x64)"; exit 0; }
	@test -f $(LLAMA_GGUF) || { echo "  (skip: $(LLAMA_GGUF) not found)"; exit 0; }
	$(CC) -O2 -std=c11 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-macro-redefined -Wno-format \
	    -I core -I $(LLAMA_INC) -o $(BUILD)/gguf_graft_belt tools/gguf_graft_belt.c \
	    $(LLAMA_DLL)/llama.dll $(LLAMA_DLL)/ggml.dll $(LLAMA_DLL)/ggml-base.dll \
	    $(LLAMA_DLL)/ggml-cpu-x64.dll -lzstd -lm
	DWGLS_GGUF="$(LLAMA_GGUF)" DWGLS_LLAMA_DLL="$(LLAMA_DLL)" \
	  PATH="$(LLAMA_DLL):$$PATH" ./$(BUILD)/gguf_graft_belt "$(LLAMA_GGUF)" "The capital of France is" 40

# ── Page field: tokenizer KV → field, graft header 5.9MB → ~20KB ──
# Tokenizer strings live in the window chain; the graft header only carries
# pointer keys. Serve = materialize full GGUF from the field and generate.
graft-page: | $(BUILD)
	@test -f $(LLAMA_DLL)/llama.dll || { echo "  (skip: llama DLLs not found — needs I:/llama/llama-b9733-bin-win-vulkan-x64)"; exit 0; }
	@test -f $(LLAMA_GGUF) || { echo "  (skip: $(LLAMA_GGUF) not found)"; exit 0; }
	$(CC) -O2 -std=c11 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-macro-redefined -Wno-format \
	    -I core -I $(LLAMA_INC) -o $(BUILD)/gguf_graft_page tools/gguf_graft_page.c \
	    $(LLAMA_DLL)/llama.dll $(LLAMA_DLL)/ggml.dll $(LLAMA_DLL)/ggml-base.dll \
	    $(LLAMA_DLL)/ggml-cpu-x64.dll -lzstd -lm
	PATH="$(LLAMA_DLL):$$PATH" ./$(BUILD)/gguf_graft_page $(LLAMA_GGUF) "The capital of France is" 40

# ── Lazy serve: KV in memory, field windows mmap'd on demand ──
# No 670MB materialization: gguf_init_from_buffer(no_alloc) parses the KV,
# llama_model_init_from_user's callback ZERO-COPIES (repoints t->data into
# the field mmap — pages fault in when ggml reads them at generation, not at
# load). Measures windows touched / page faults / residency per phase.
lazy-serve: | $(BUILD)
	@test -f $(LLAMA_DLL)/llama.dll || { echo "  (skip: llama DLLs not found — needs I:/llama/llama-b9733-bin-win-vulkan-x64)"; exit 0; }
	@test -f $(LLAMA_GGUF) || { echo "  (skip: $(LLAMA_GGUF) not found)"; exit 0; }
	$(CC) -O2 -std=c11 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-macro-redefined -Wno-format \
	    -I core -I $(LLAMA_INC) -o $(BUILD)/gguf_lazy_serve tools/gguf_lazy_serve.c \
	    $(LLAMA_DLL)/llama.dll $(LLAMA_DLL)/ggml.dll $(LLAMA_DLL)/ggml-base.dll \
	    $(LLAMA_DLL)/ggml-cpu-x64.dll -lzstd -lpsapi -lm
	PATH="$(LLAMA_DLL):$$PATH" ./$(BUILD)/gguf_lazy_serve $(LLAMA_GGUF) "The capital of France is" 40

# ── FGLS_vis: geometry visualizer + console ─────────────
GGUF ?= I:/model/SmolLM2-360M-Instruct.Q8_0.gguf

vis: $(BUILD)/gguf_tool.exe
	@echo "▶ FGLS_UI → http://127.0.0.1:5001  (GGUF: $(GGUF))"
	python tools/fgls_vis.py 5001 $(GGUF)

$(BUILD)/gguf_tool.exe: tools/gguf_tool.c tools/gguf_dump.c core/gguf_reader.h | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/gguf_tool.exe tools/gguf_tool.c $(LDFLAGS)

# ── geo_net Barrett shift-mod6 probe (correctness domain + cycles + labels — manual) ──
geo_net_probe: tools/geo_net_probe.c | $(BUILD)
	@echo "▶ BUILD  geo_net_probe"
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -o $(BUILD)/geo_net_probe tools/geo_net_probe.c
	@echo "✅ geo_net_probe ready → ./$(BUILD)/geo_net_probe"

# ── GeoSeed 2-register topology extraction probe (12-face labels, shift+mask — manual) ──
seed_label_probe: tools/seed_label_probe.c | $(BUILD)
	@echo "▶ BUILD  seed_label_probe"
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -o $(BUILD)/seed_label_probe tools/seed_label_probe.c
	@echo "✅ seed_label_probe ready → ./$(BUILD)/seed_label_probe"

# ── T1.1b scale-predict → residual → gradient (manual) ──
scale_residual: tools/scale_residual_probe.c core/gguf_reader.h | $(BUILD)
	@echo "▶ BUILD  scale_residual_probe"
	$(CC) $(CFLAGS) -o $(BUILD)/scale_residual_probe tools/scale_residual_probe.c $(LDFLAGS)
	@echo "✅ scale_residual ready → ./$(BUILD)/scale_residual_probe --syn smooth|sine2d|noise <n> | --file <path> [cols] | --gguf <model> <tensor_idx>"

# ── T1.2 field trainer: evolutionary search เหนือ integer knobs (manual) ──
field_trainer: tools/field_trainer.c core/geo_ghost_envelope.h core/gguf_reader.h | $(BUILD)
	@echo "▶ BUILD  field_trainer"
	$(CC) $(CFLAGS) -o $(BUILD)/field_trainer tools/field_trainer.c $(LDFLAGS)
	@echo "✅ field_trainer ready → ./$(BUILD)/field_trainer --gguf <model> [--gens N] [--pop N] [--seed S] [--eval s,o,g,O,chunk]"

# ── T1.1c scale-predict residual ใน delta log (manual) ──
delta_log_residual: tools/delta_log_residual.c core/gguf_reader.h | $(BUILD)
	@echo "▶ BUILD  delta_log_residual"
	$(CC) $(CFLAGS) -o $(BUILD)/delta_log_residual tools/delta_log_residual.c $(LDFLAGS)
	@echo "✅ delta_log_residual ready → ./$(BUILD)/delta_log_residual --file <path> | --syn <kind> <n> | --gguf <model> <idx>"

# ── T1.1d Huffman จริงบน residual — เทียบ entropy bound (manual) ──
huff_delta_measure: tools/huff_delta_measure.c core/huff_codec.h core/gguf_reader.h | $(BUILD)
	@echo "▶ BUILD  huff_delta_measure"
	$(CC) $(CFLAGS) -o $(BUILD)/huff_delta_measure tools/huff_delta_measure.c $(LDFLAGS)
	@echo "✅ huff_delta_measure ready → ./$(BUILD)/huff_delta_measure --file <path> | --gguf <model> <idx> | --syn <kind> <n>"

# ── T1.2 wire champion knobs เข้า chain จริง — lossless byte-for-byte (manual) ──
cap_chain_scan: tools/cap_chain_scan.c core/geo_cap_account.h core/geo_ghost_lift.h core/geo_ghost_envelope.h | $(BUILD)
	@echo "▶ BUILD  cap_chain_scan"
	$(CC) $(CFLAGS) -o $(BUILD)/cap_chain_scan tools/cap_chain_scan.c $(LDFLAGS)
	@echo "✅ cap_chain_scan ready → ./$(BUILD)/cap_chain_scan <folder> | --gguf <model> [--stride S] [--offset O] [--gate G] [--orbit Q] [--chunk C]"

# ── T1.3 Triangular addressing probe — Nagy 2003/2004 B-distance vs stride-37 (manual) ──
triangular_addressing_probe: tools/triangular_addressing_probe.c | $(BUILD)
	@echo "▶ BUILD  triangular_addressing_probe"
	$(CC) $(CFLAGS) -o $(BUILD)/triangular_addressing_probe tools/triangular_addressing_probe.c $(LDFLAGS)
	@echo "✅ triangular_addressing_probe ready → ./$(BUILD)/triangular_addressing_probe"

# ── T1.3b lane addressing + rotation theorem + GGUF rule-cost (manual) ──
lane_field_probe: tools/lane_field_probe.c core/gguf_reader.h core/geo_ghost_envelope.h | $(BUILD)
	@echo "▶ BUILD  lane_field_probe"
	$(CC) $(CFLAGS) -o $(BUILD)/lane_field_probe tools/lane_field_probe.c $(LDFLAGS)
	@echo "✅ lane_field_probe ready → ./$(BUILD)/lane_field_probe [--gguf <model>]"

# ── §15.73/§15.78 wire CAP_RULE_* + walk-based read เข้า geofs read path —
# placement/admit/read กฎเดียว + อ่าน = เดินนาฬิกา (enter-anywhere) (manual) ──
rule_e2e: tools/rule_e2e.c core/geofs_core.h core/geo_ghost_lift.h core/geo_cap_account.h core/fibo_walk.h | $(BUILD)
	@echo "▶ BUILD  rule_e2e"
	$(CC) $(CFLAGS) -o $(BUILD)/rule_e2e tools/rule_e2e.c $(LDFLAGS)
	@echo "✅ rule_e2e ready → ./$(BUILD)/rule_e2e [file ≤4MB]"

# ── §15.74 delta-mode ghost: pred+ent เข้า ghost_read_rule + วัด footprint (manual) ──
ghost_delta_measure: tools/ghost_delta_measure.c core/ghost_delta.h core/geo_ghost_lift.h core/geo_cap_account.h | $(BUILD)
	@echo "▶ BUILD  ghost_delta_measure"
	$(CC) $(CFLAGS) -o $(BUILD)/ghost_delta_measure tools/ghost_delta_measure.c $(LDFLAGS)
	@echo "✅ ghost_delta_measure ready → ./$(BUILD)/ghost_delta_measure --file <path> | --syn <kind> <n> | --gguf <model> <idx>"

# ── §15.75/§15.77 tied-embedding dedup + walk-based access: registry {id→home} —
# byte-identical tensors freeze ครั้งเดียว · walk นาฬิกาหา route ที่ live เหนือ dedup field ──
tied_dedup: tools/tied_dedup_chain.c core/tied_dedup.h core/fibo_walk.h core/gguf_box.h | $(BUILD)
	@echo "▶ BUILD  tied_dedup_chain"
	$(CC) $(CFLAGS) -o $(BUILD)/tied_dedup_chain tools/tied_dedup_chain.c $(LDFLAGS)
	@echo "✅ tied_dedup ready → ./$(BUILD)/tied_dedup_chain <model.gguf> [--dedup|--no-dedup|--both] [--no-walk]"

goldberg_probe: tools/goldberg_dual_probe.c core/geo_goldberg_store.h core/geo_goldberg_file.h core/geo_goldberg_decagram.h core/geo_goldberg_sphere.h core/infra/tring.h core/gguf_box.h core/geo_param_grid.h | $(BUILD)
	@echo "▶ BUILD  goldberg_dual_probe"
	$(CC) $(CFLAGS) -o $(BUILD)/goldberg_dual_probe tools/goldberg_dual_probe.c $(LDFLAGS)
	@echo "✅ goldberg_probe ready → ./$(BUILD)/goldberg_dual_probe <model.gguf> [--all]"

ggf_bench: tools/ggf_mmap_bench.c core/geo_goldberg_file.h core/geo_goldberg_store.h core/geo_goldberg_decagram.h core/geo_goldberg_sphere.h core/infra/tring.h | $(BUILD)
	@echo "▶ BUILD  ggf_mmap_bench"
	$(CC) $(CFLAGS) -o $(BUILD)/ggf_mmap_bench tools/ggf_mmap_bench.c $(LDFLAGS)
	@echo "✅ ggf_bench ready → ./$(BUILD)/ggf_mmap_bench <file.ggf>"

kv-park-bench: tools/kv_park_bench.c core/kv_geofs_bridge.h core/kv_remap.h core/kv_remap_rail.h core/geofs_core.h | $(BUILD)
	@echo "▶ BUILD  kv_park_bench"
	$(CC) $(CFLAGS) -o $(BUILD)/kv_park_bench tools/kv_park_bench.c $(LDFLAGS)
	@echo "✅ kv-park-bench ready → ./$(BUILD)/kv_park_bench"

kv-real-multiturn: tools/kv_real_multiturn_bench.c core/kv_dramtile_bridge.h core/kv_remap.h core/dramtile_store.h core/dramtile_store.c | $(BUILD)
	@echo "▶ BUILD  kv_real_multiturn_bench"
	$(CC) $(CFLAGS) -o $(BUILD)/kv_real_multiturn_bench tools/kv_real_multiturn_bench.c core/dramtile_store.c $(LDFLAGS)
	@echo "✅ kv-real-multiturn ready → ./$(BUILD)/kv_real_multiturn_bench <kvslots dir>"

ggf_ckpt: tools/ggf_checkpoint_replay.c core/geo_ggf_ckpt.h core/geo_ggf_walk.h core/geo_goldberg_file.h core/tied_dedup.h core/gguf_box.h | $(BUILD)
	@echo "▶ BUILD  ggf_checkpoint_replay"
	$(CC) $(CFLAGS) -o $(BUILD)/ggf_checkpoint_replay tools/ggf_checkpoint_replay.c $(LDFLAGS)
	@echo "✅ ggf_ckpt ready → ./$(BUILD)/ggf_checkpoint_replay <model.gguf> --ckpt-dir <dir>"

ggf_fs: tools/ggf_fs_probe.c core/geo_ggf_fs.h core/geo_ggf_ckpt.h core/geo_ggf_walk.h core/geo_goldberg_file.h | $(BUILD)
	@echo "▶ BUILD  ggf_fs_probe"
	$(CC) $(CFLAGS) -o $(BUILD)/ggf_fs_probe tools/ggf_fs_probe.c $(LDFLAGS)
	@echo "✅ ggf_fs ready → ./$(BUILD)/ggf_fs_probe --mount <ckpt-dir> [--sweep r1 t1 r2 t2]"

