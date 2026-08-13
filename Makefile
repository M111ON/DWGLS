# DWGLS Makefile — 4D Geometry + KIS Timeline
# ═══════════════════════════════════════════════
# Usage:
#   make test          — compile + run all tier-1 tests
#   make test-TIER1    — same as above
#   make test-NAME     — compile + run single test
#   make clean         — remove build artifacts
#   make list          — list available tests

CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -I. -Icore -Icore/infra -Icore/infra
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
  test_geo_inference \
  test_geo_sid_loader \
  test_geo_prune \
  test_geo_fs \
  test_geo_fs_mdim \
  test_monitor \
  test_phi_microscope \
  test_safetensors_reader \
  test_tess_index_frame \
  test_tess_scale_log \
  test_tess_frame_seek \
  test_tess_magnify \
  test_tess_hex_delta \
  test_bfs_persist \
  test_bfs_stability \
  test_geo_bfs_hub \
  test_bfs_seek_anchor \
  test_bfs_breath

# ── Tier 2: need gguf_reader.h or geo_frame_seek.h ────
# (removed: kis_codec_v5_test, kis_codec_v6_test, kis_map_roundtrip,
#  kis_real_gguf_test, test_geo_sid_verify, test_rail_hub,
#  test_qwen3_microscope, test_real_gguf_microscope — cross-repo deps, hang on fail)
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

# ── FGLS_vis: geometry visualizer + console ─────────────
GGUF ?= I:/model/SmolLM2-360M-Instruct.Q8_0.gguf

vis: $(BUILD)/gguf_tool.exe
	@echo "▶ FGLS_UI → http://127.0.0.1:5001  (GGUF: $(GGUF))"
	python tools/fgls_vis.py 5001 $(GGUF)

$(BUILD)/gguf_tool.exe: tools/gguf_tool.c tools/gguf_dump.c core/gguf_reader.h | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/gguf_tool.exe tools/gguf_tool.c $(LDFLAGS)
