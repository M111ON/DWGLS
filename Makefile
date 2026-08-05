# DWGLS Makefile — 4D Geometry + KIS Timeline
# ═══════════════════════════════════════════════
# Usage:
#   make test          — compile + run all tier-1 tests
#   make test-TIER1    — same as above
#   make test-NAME     — compile + run single test
#   make clean         — remove build artifacts
#   make list          — list available tests

CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -I. -Icore -Icore/infra
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
  test_monitor \
  test_phi_microscope \
  test_safetensors_reader

# ── Tier 2: need gguf_reader.h or geo_frame_seek.h ────
TIER2 := \
  kis_codec_v4_test \
  kis_codec_v5_test \
  kis_codec_v6_test \
  kis_map_roundtrip \
  kis_real_gguf_test \
  test_geo_sid_verify \
  test_geo_tensor_hub \
  test_qwen3_microscope \
  test_real_gguf_microscope

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
test: tier1
	@echo "═══════════════════════════════════════"
	@echo "TIER1: $(words $(TIER1)) tests compiled + run"
	@echo "TIER2: $(words $(TIER2)) tests (need gguf_reader.h)"
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

tier2:
	@echo "Tier 2 tests (need gguf_reader.h — not in DWGLS yet):"
	@for t in $(TIER2); do echo "  ⏸ $$t"; done

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
	@echo "make clean      — remove build/"
	@echo "make list       — list all tests"
