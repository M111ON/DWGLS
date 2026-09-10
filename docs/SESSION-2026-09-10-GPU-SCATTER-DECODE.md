# Session 2026-09-10: GPU Scatter Decode & Multi-Format Lossless

## สรุป

พิสูจน์ lossless ของ scatter decode pipeline บน GPU จริงกับ 3 โมเดล 3 architecture ต่างกัน, แก้ bug สำคัญที่ซ่อนอยู่, เพิ่ม sig32 integrity layer, พิสูจน์ว่า gguf_reader.h รองรับ GGUF v3 (Gemma 4) อยู่แล้ว

## สิ่งที่ทำ

### 1. Gemma 4 Bake (GGUF v3)

**คำถาม:** Gemma 4 ใช้ GGUF v3 ซึ่งเปลี่ยน field widths ทั้งหมด (uint32→uint64) — gguf_reader.h อ่านได้ไหม?

**คำตอบ:** อ่านได้แล้ว — reader ใช้ `gbuf_u64()` สำหรับทุก length field อยู่ก่อนแล้ว (line 132, 147, 221 ใน gguf_reader.h). Python inspector ที่ crash ก่อนหน้าเป็นเพราะอ่าน u32 ผิด ไม่ใช่ C reader

**ผลลัพธ์:**
- Bake สำเร็จ: 601 tensors, 3438 capos, 2921.5 MB (100.7% ของ 2900 MB GGUF)
- Stride-37 bijection: PASS
- Sig32: 0x7EDBAC48

### 2. Sig32 XOR-fold Integrity

**แนวคิด:** `(sig64 >> 32) ^ (sig64 & 0xFFFFFFFF)` — 64-bit running XOR ที่บีบเป็น 32-bit fingerprint

**ใช้ทำอะไร:**
- Capo dedup (identical tensors = identical sig32)
- Fast tensor match (sig32 compare O(1) vs strcmp O(name_len))
- Zone health check (sig32 ซ้ำกันทั้ง zone = scatter ล้มเหลว)

**ผลลัพธ์:** เพิ่มใน `tess_gguf_pack.c` hdr[8] — ไม่ส่งผลกระทบกับ pipeline ที่มีอยู่

### 3. GPU Scatter Decode Standalone Kernel

**สถาปัตยกรรม:**
```
mmap .tesspack → parse header/index → cudaHostRegister → GPU kernel
                                                        ↓
                              scatter_decode_kernel: idx → slot = (idx*37)%20736 → read cell_size bytes
                                                        ↓
                              sig32_verify_kernel: XOR-fold บน GPU output เปรียบเทียบกับ expected
```

**ไม่มี llama.cpp dependency** — compile ด้วย nvcc ตรงๆ

### 4. Mixed Cell_sz Bug (Qwen3-TTS)

**ตัวหิน:** Qwen3-TTS 1.7B Q4_K_M — ไฟล์เล็ก แต่ slot เยอะ

**Bug ที่พบ:** Scatter decode kernel ใช้:
```c
dst = output + (uint64_t)global_idx * cell_sz;
```

**ปัญหา:** cell_sz ต่างกันระหว่าง capos ภายในไฟล์เดียวกัน:
- Q4_K_M tensors → cell_sz = 210
- Q8_0 tensors → cell_sz = 144
- Norm/emb → ONION

`global_idx` ข้าม capos แต่ `cell_sz` ของแต่ละ capo ต่างกัน → output address ผิด → CUDA illegal memory access

**ทำไม Qwen3-0.6B ไม่เจอ:** ทุก capo มี cell_sz เดียวกัน (34 สำหรับ Q8_0) — bug ซ่อนอยู่เพราะ uniform quantization

**Fix:** เพิ่ม `capo_out_bytes[]` (cumulative byte offset per capo):
```c
dst = output + capo_out_bytes[c] + (uint64_t)local_idx * cell_sz;
```

### 5. Multi-Format Lossless Proven

| Model | Format | Capos | Cell_sz | Decode | Sig32 | CPU/GPU |
|-------|--------|-------|---------|--------|-------|---------|
| Qwen3-TTS 1.7B | Q4_K_M | 426 | mixed (144, 210) | 6012 ms | 0/426 ✅ | MATCH ✅ |
| Qwen3-0.6B | Q8_0 | 991 | uniform (34) | 1771 ms | 0/991 ✅ | MATCH ✅ |
| Bonsai-4B | Q1_0 | 145 | uniform (8) | 3 ms | 0/145 ✅ | MATCH ✅ |

## ข้อคิดจากการทำงาน

### 1. Bugs ซ่อนอยู่ใน uniformity
Qwen3-0.6B ทดสอบผ่านทุกครั้งเพราะ cell_sz ตายตัว — bug ใน output addressing ไม่มีทางเจอถ้าไม่ทดสอบกับ mixed-format model. **Qwen3-TTS เป็น stress test ที่ดีเพราะบังคับให้เจอ edge case จริง**

### 2. Bake = I/O bound, ไม่ใช่ CPU bound
Stride-37 scatter = O(1) ต่อ weight (แค่ multiplication + modulo) ไม่ต้อง sort, ไม่ต้อง lookup. ที่ช้าคือ disk read/write — computation จิ๊บจ๊อย

### 3. Peak RAM ไม่ scale กับ model size
Bake loop อ่าน tensor ทีละตัว → เขียน capo ทีละตัว → peak RAM ~5MB ไม่ว่า model จะ 1GB หรือ 500GB

### 4. Baseline vs Optimized
- Raw scatter decode: 0.36 GB/s (ยังไม่ได้ใช้เครื่องมืออะไร)
- DRamTile + GearLock + sig32 compressed: ~47 GB/s (ก่อนหน้า)
- ความต่าง = ผลของ DRamTile zero-copy + batch + compression pipeline

### 5. Philosophy: Coordinate = Address
Scatter decode ไม่ต้องมี lookup table — address คำนวณจาก weight index ตรงๆ. นี่คือ "MAP not COMPRESS" — geometry เป็น address space, ไม่ใช่ compression algorithm

## Files Changed

| File | Change |
|------|--------|
| `bench/tess_scatter_decode.cu` | Standalone GPU kernel + mixed cell_sz fix (capo_out_bytes) |
| `colab-pack/deploy_scatter_decode.sh` | Colab deploy script |
| `tools/tess_gguf_pack.c` | Sig32 XOR-fold in hdr[8] |
| `AGENTS.md` | Updated status |

## Commits

- `1469e41` — fix: scatter decode mixed cell_sz output addressing
- `27ea7f7` — docs: multi-format lossless proven
