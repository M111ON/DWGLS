# MULTIDIM-READ-PLAN — 2026-11-18 (core multi-dim zero-copy storage)

## เป้าหมาย (core intent, #6192 #7159 #7530)
multi-dimensional storage ที่หยิบ/อ่านข้อมูลจากมิติที่สูงกว่า (w/tesseract, t/scale-timeline) ได้แบบ zero-copy ไม่ต้องย้าย payload

## ทำแล้ว (primitive)
- field roundtrip fold/scale/reshape memcmp 100% (5 data types, code เดียวกัน)
- bijective maps (stride-37, hj3, hj4) orbit count verified
- multi-view flat/rail/time/cell (state_at free via journal)
- tesseract w-dim transform (tess_flat/tess_unflat) identity 20736/20736
- zero-copy pointer (GGUFBox, tesspack ONION path)
- BreathingFS mmap (3.5 GB / 48 MB RSS)
- tests/test_tess_wdim_read.c: ทุก check PASS (roundtrip + w-dim read memcmp + neg)

## ทำแล้ว (increment 1)
- `tools/field_resolve.c` + Makefile target `field-resolve`
- universal resolver: อ่าน body_off + fpos จาก field's own KV (ไม่ต้อง source GGUF) → zero-copy pointer + tesseract (tess,cell,slot) coord
- verified บน fieldA.bin: 291/291 in-bounds (selfcheck) + 291/291 oracle-verified (memcmp vs source Qwen2.5-0.5B)

## ทำแล้ว (increment 2 — DONE)
- `tools/gguf_lazy_serve.c`: wire tesseract (w-dim) coord เข้า consumer resolution path
  - include `core/geo_tesseract_addr.h`
  - `provide_tensor`: คำนวณ tess coord ของ slot ที่ bind (gated by `GGUF_WDIM_LOG`)
  - startup self-check: ทุก tensor roundtrip identity ผ่าน tess_unflat→tess_flat
- runtime verified: `make lazy-serve` → "w-dim selfcheck: 291/291 tensors roundtrip identity", FINAL 12/12 PASS

## ทำแล้ว (increment 3)
- `tests/test_clim_shift.c`: climate change ที่ scale — 20736 slots apply+invert identical บน consumer A และ B, window move + successive slide คง sync, 11/11 PASS
- wired into Makefile TIER1 (`make test-test_clim_shift`)
- `tools/field_resolve.c`: เพิ่ม `--clim <file>` climate awareness — load+verify CLIM, apply capo shift, roundtrip check ต่อ tensor; verified 291/291 บน fieldA.bin (offset=123456789)
- build verified: `make field-resolve`

## ทำแล้ว (increment 4)
- `tests/test_concurrent_mdim_read.c`: 4 threads x 291 tensors concurrent w-dim resolve + zero-copy read, 0 fails ทุก thread, PASS
- wired into Makefile TIER1 (`make test-test_concurrent_mdim_read`)

## increments ถัดไป (เรียงตามคุณค่า)
1. ✅ universal resolver — DONE
2. ✅ w-dim reads เข้า consumer จริง — DONE (self-check 291/291, lazy-serve 12/12)
3. ✅ climate change ที่ scale — DONE (scale + multi-consumer proven)
4. ✅ concurrent multi-dim reads — DONE (4 threads, 0 fails)

## ไม่ควรทำตอนนี้
- scale b_fs / Zone-2 ก่อนพิสูจน์ (1)-(2)
- แข่ง vLLM ใน compute/batching (ride on top แทน)
- convert nomic embedder เป็น tesspack (ไม่คุ้ม)