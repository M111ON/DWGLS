# รายงานสถานะ DWGLS Pipeline — 2026-08-10 (ตามจริง ตรงไปตรงมา)

> รายงานนี้เขียนจากผลวัดจริงทุกจุด ไม่มีการประมาณในส่วนที่วัดไม่ได้
> ภาษา: ไทย (technical terms เป็นภาษาอังกฤษ)

---

## 1. สรุปภาพรวม (Executive Summary)

เป้าหมาย Phase 2 A: พิสูจน์ chain การทำงานจริง
`GGUF → bake → .gcube → mmap → rail → llama.cpp → GPU inference`

**สิ่งที่สำเร็จจริงในวันนี้:**
1. ✅ `.gcube` bake แบบ GGUF-direct — 291/291 tensors lossless, ขนาดไฟล์เท่าเดิมเป๊ะ
2. ✅ Inference ผ่าน prebuilt Vulkan (llama-b9733) — 46.9-61.6 t/s รันจริง, 81.6 t/s (bench)
3. ✅ พิสูจน์ GPU ทำงานจริง: 2× GTX 1050 Ti ใช้ทั้งคู่ (93%/100% util ที่ workload 400 tokens)
4. ✅ เจอต้นเหตุ "CUDA หาย" — PATH ชี้เวอร์ชันที่ไม่มี, ตัวจริงอยู่ I:\cuda_temp (12.6.20)
5. ✅ nvcc + MSVC ใช้ได้ — compile CUDA smoke test ผ่าน, kernel รันบน GPU จริง
6. ✅ JetBridge (DRamTile×GearLock×FiboSpine) รันบนเครื่องนี้ครั้งแรก — 49.6 GB/s, 0 errors

**สิ่งที่ยังไม่ได้ / ยังไม่จริง:**
- ❌ inference ยังเป็น vanilla llama.cpp Vulkan offload — **ไม่ใช่** pipeline ของเรา
- ❌ JetBridge พิสูจน์ได้แค่ bandwidth pull (ไม่ใช่ matmul inference)
- ❌ ยังไม่มี kernel matmul ของเราเองที่ทำ inference จริง
- ❌ 2 GPU ยังไม่ได้ใช้ผ่าน chain ของเรา (llama จัดการเอง)

---

## 2. RAM — วัดจริง

เครื่อง: 8GB (7.94 GB), CPU Pentium G4400 (2 cores, no AVX2), 2× GTX 1050 Ti 4GB

| Phase | peak RSS | หมายเหตุ |
|---|---|---|
| bake (GGUF-direct) | 1,649 MiB | transient — อ่าน source + blocks ใน RAM + verify |
| inference (Vulkan) | 906 MiB | mmap 644MB (reclaimable) + KV cache + compute |
| GPU0 VRAM | 841 MiB peak | 400-token run |
| GPU1 VRAM | 921 MiB peak | 400-token run |

**RAM รวมตอนว่าง:** 4.05-4.31 GB free / 7.94 GB (51% ว่าง) หลังปิด WSL
- ก่อนหน้านี้ WSL2 ยืน commit ~3.4 GB → `wsl --shutdown` คืน RAM
- ไม่มี memory leak ใน pipeline ของเรา (bake จบแล้วปล่อย, inference mmap reclaimable)

---

## 3. GPU — พิสูจน์ว่าทำงานจริง (A/B test)

binary เดียว (llama-b9733 win-vulkan), prompt เดียว, 31 tokens gen:

| Config | prompt | eval |
|---|---|---|
| `-ngl 0` (CPU) | 18.89 t/s | 7.98 t/s |
| `-ngl 99` (Vulkan) | 267.32 t/s | 50.81 t/s |
| Speedup | **14.1x** | **6.4x** |

**ทำไม nvidia-smi ก่อนหน้าเห็น 0%:** workload สั้น (~300ms/run) sampling 100ms พลาด burst
**พิสูจน์ด้วย workload ยาว:** 400 tokens, sampling 25ms → GPU0 util 93%, GPU1 util 100%

llama shard weights อัตโนมัติข้าม 2 GPU (GPU0 841 + GPU1 921 MiB ≈ 1.7GB)

**llama-bench (benchmark ไม่ใช่ sampling):** tg64 = **81.58 ± 7 t/s**, pp128 = 997 t/s

---

## 4. การเดินทางของโค้ด — สิ่งที่เกิดขึ้นจริง (ตามลำดับ)

### 4.1 L0 (ก่อนวันนี้, commit 7dbf6fb)
`.gcube → re-emit GGUF → llama-completion` — inference ครั้งแรกสำเร็จ output ถูกต้อง (2+2 = 4)

### 4.2 L1 — callback experiment (วันนี้, **FAILED — เปลี่ยนทาง**)
พยายามทำ "zero re-emit": patch llama.cpp ให้โหลดจาก .gcube ตรงผ่าน `LOAD_MODE_NONE` + `set_tensor_data`

**สิ่งที่ค้นพบ (root cause ที่แท้จริง):**
- `files.empty()` branch ใน `llama-model-loader.cpp` สร้าง tensor ที่ NOT_REQUIRED ทุกตัว
  แม้ไม่มีใน metadata → `*.scale` ถูกสร้างเป็น 0 → graph เอา scale=0 ไปคูณ attention → garbage
- patch loader (return nullptr สำหรับ NOT_REQUIRED ที่ไม่มีใน metadata) — **ทำแล้ว พิสูจน์แล้วว่า logits ตรงกับ GGUF ทุกตัว**
- **แต่ทิ้ง path นี้** — เพราะมันซับซ้อน + ต้อง maintain fork llama.cpp

### 4.3 GGUF-direct bake (วันนี้, **WINNER**)
แนวคิด user: "เอาหัวเป็น gguf แล้วเราก็เป็นฐานใน gguf — แปลงแค่ครึ่งตัว"

`bake_gcube --gguf`:
- เขียน GGUF header + tensor index **byte-identical** จาก source
- เขียน data จาก blocks ของเรา (blocks เก็บ byte ตรงกับ GGUF data เป๊ะ)
- ผล: **291/291 lossless, IDENTICAL SIZE (644.41 MB)**
- ไฟล์เดียว 2 consumer: llama เปิดเป็น GGUF standard (mmap+Vulkan) / rail ของเราอ่าน offset

**ทำไมดีกว่า:** ไม่ต้อง fork llama.cpp, ไม่ต้อง custom loader, GPU path ที่ proven แล้วทำงาน 100%
**ราคาที่จ่าย:** bake ~2.4-3.8s (write) + verify 0.5s — transient เท่านั้น

### 4.4 Vulkan build บนเครื่องนี้ (วันนี้)
- WSL: shader compile fail (`rwkv_wkv7_f32`, `acc_f32`) → WSL ใช้ GPU ไม่ได้ (ยืนยัน)
- MSYS2 native: configure + ninja build สำเร็จ (1,871 shaders), gcube_run_vk.exe compile ได้
- **SIGILL ตอนรัน** — GGML_AVX2=ON ใน build ทั้งที่ G4400 ไม่มี AVX2 (GGML_NATIVE=OFF ไม่ปิด AVX2 flag อัตโนมัติ)
- สรุป: build นี้พับ — ไม่จำเป็น เพราะ GGUF-direct ใช้ prebuilt binary ที่ทำงานแล้ว

### 4.5 CUDA — ปริศนา "หายตลอด" ถูกแก้ (วันนี้)
**พบ:** ผู้ใช้เคยลง CUDA หลายครั้งแต่ `nvcc` หาไม่เจอตลอด

**ต้นเหตุ (ตรวจ PATH จริง):**
```
PATH machine ชี้ไป:
  C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\bin  ← ไม่มีจริง (โฟลเดอร์ไม่ exist)
  C:\Program Files\...\CUDA\v12.6\bin                          ← มีแค่ folder "extras" (ไม่ใช่ toolkit เต็ม!)
จริงที่มี: I:\cuda_temp = CUDA 12.6.20 เต็ม (nvcc 12.6.20, ใช้ได้)
```
→ PATH ชี้เวอร์ชันที่ไม่มี/ไม่สมบูรณ์ = "nvcc หาย" ทุกครั้งที่โปรแกรมเรียกผ่าน PATH

**แก้:** ลบ v13.2 (dead) + v12.6 (extras-only) ออกจาก Machine PATH → เหลือ `I:\cuda_temp\bin`

**MSVC:** พบ Build Tools 2022 (14.44.35207) ที่ `C:\Program Files (x86)\...\BuildTools` — cl.exe ใช้ได้

**พิสูจน์:**
- `nvcc --version` → V12.6.20 ✅
- compile `cuda_smoke.cu` (sm_61) → kernel รันบน 1050 Ti จริง ✅

### 4.6 JetBridge กลับมามีชีวิต (วันนี้)
`gpu_jet_puller*.cu` (5 ไฟล์) ที่ I:\FGLS_new/runner/gpu_jet_puller/ — เคย compile ไม่ได้เพราะไม่มี nvcc

- `build_simple.bat` มีอยู่ (ชี้ vcvarsall x64 + sm_61) → รันผ่าน ✅
- `gpu_jet_puller_local.exe` วิ่ง: 4,147 bridges, 7,166,016 pulls, **0 errors**
- `bench_opt` ต้องแก้ 2 บรรทัด (designated init `{.c144_ref=NULL}` → memset — MSVC compat)

**ผล benchmark (1050 Ti, kernel จริง):**
| chunk | with XOR | no XOR |
|---|---|---|
| 64B | 3.19 GB/s | 3.27 GB/s |
| 256B | 12.82 GB/s | 12.94 GB/s |
| 1024B | **49.60 GB/s** | 47.65 GB/s |

**เทียบ bandwidth:** 49.6 GB/s (CUDA JetBridge) > 29.2 GB/s (Vulkan puller ก่อนหน้า)
**แปลเป็น tokens/s:** 0.5B-Q8 ≈ 505MB/token → 49.6GB/s ÷ 505MB ≈ **~98 t/s ceiling** (bandwidth เดียว, ยังไม่รวม matmul)

---

## 5. ตัวเลขสำคัญรวม (ทุกอย่างวัดจริง)

| ตัวชี้วัด | ค่า | เงื่อนไข |
|---|---|---|
| bake lossless | 291/291 | byte-identical ตรวจทุก tensor |
| ขนาดไฟล์ | 644.41 MB = ของเดิมเป๊ะ | IDENTICAL SIZE |
| eval t/s (รันจริง 400 tok) | 46.9 | completion + sampling |
| eval t/s (benchmark) | 81.58 ± 7 | llama-bench tg64 |
| prompt t/s | 267-450 | Vulkan |
| CPU เทียบ | 7.98 eval | -ngl 0 |
| speedup GPU/CPU | 6.4-14x | |
| JetBridge BW | 49.60 GB/s | chunk 1024B |
| JetBridge errors | 0 | 7.16M pulls |
| DWGLS tests | 27/27 + 4/4 | make test |
| RAM ว่าง (หลัง WSL off) | 4.05-4.31 GB | ของ 7.94 |

---

## 6. สิ่งที่ยังไม่จริง / ข้อจำกัด (พูดตรงๆ)

1. **Inference ยังเป็น vanilla Vulkan offload** — ไม่ใช่ geo_rail_hub→DRamTile→GearLock→JetBridge→kernel ที่ user ตั้งเป้า
2. **JetBridge เป็น bandwidth pull เท่านั้น** — ยังไม่มี matmul kernel ที่ทำ inference
3. **"~98 t/s ceiling" เป็นการคำนวณจาก bandwidth** ไม่ใช่ผลวัดของ inference จริง — การดึงข้อมูลผ่านตัวเดียวกับ matmul จริงคนละเรื่อง
4. **2 GPU ทำงานผ่าน llama ไม่ใช่ผ่านเรา** — chain ของเรายัง single-device (sm_61)
5. **GCUBE format เดิม (GCB) กับ GGUF-direct ต่างกัน** — ของเดิมยังใช้กับ rail ได้, GGUF-direct ใช้กับ llama; ยังไม่ unified format เต็มรูปแบบ
6. **WSL ใช้ GPU ไม่ได้** (shader compile fail) — งาน GPU ทั้งหมดต้อง native Windows
7. **บิลด์ Vulkan ของเรา SIGILL บน G4400** — ต้องใช้ prebuilt binary (b9733) ตอนนี้; ถ้าจะ build เองต้องตั้ง GGML_AVX2=OFF
8. **loader patch (4.2) ทำแล้วแต่ไม่ได้ใช้** — เก็บเป็นความรู้, production ใช้ GGUF-direct

---

## 7. สิ่งที่พัฒนาขึ้น (ตรงไปตรงมา)

| สิ่งที่ | สถานะ | หลักฐาน |
|---|---|---|
| `bake_gcube --gguf` | ✅ เสร็จ ใช้งานได้ | 291/291 lossless, IDENTICAL SIZE |
| PATH CUDA แก้ | ✅ เสร็จ | nvcc 12.6.20 เรียกผ่าน PATH ได้ |
| nvcc+MSVC toolchain | ✅ พร้อม | smoke kernel รันบน GPU |
| JetBridge run on machine | ✅ ครั้งแรก | 49.6 GB/s, 0 err (1024B) |
| `bench_opt.cu` MSVC fix | ✅ | compile ผ่าน, รันได้ |
| loader patch (llama.cpp) | ⚠️ ทำแล้วไม่ใช้ | logits แก้ได้จริง แต่ path พับ |
| Memory evidence report | ✅ | docs/MEMORY-EVIDENCE-2026-08-10.md |
| matmul kernel ของเรา | ❌ ยังไม่มี | — |
| inference ผ่าน chain เรา | ❌ ยังไม่มี | — |
| 2-GPU ผ่าน chain เรา | ❌ ยังไม่มี | — |

---

## 8. ไฟล์ที่เกี่ยวข้อง

- `I:\DWGLS\tools\bake_gcube.c` — เพิ่ม `--gguf` mode (section 4.3)
- `I:\DWGLS\tools\gcube_token_run.c` — callback experiment (ใช้ศึกษา, ไม่ใช่ production)
- `I:\DWGLS\tools\build_gcube_run.sh` — path อัปเดต
- `I:\FGLS_new\runner\gpu_jet_puller\gpu_jet_puller_bench_opt.cu` — MSVC fix 2 บรรทัด
- `I:\DWGLS\build\cuda_smoke.cu/.exe` — CUDA smoke test (ตัวพิสูจน์ nvcc ใช้ได้)
- `I:\DWGLS\docs\MEMORY-EVIDENCE-2026-08-10.md` — report memory/GPU
- `I:\cuda_temp` — CUDA 12.6.20 (ตัวจริงที่ PATH ควรชี้)

---

## 9. ขั้นต่อไปที่เป็นไปได้ (ยังไม่ตัดสินใจ)

- **A**: port JetBridge pull → matmul kernel (sm_61) → matmul ผ่าน chain เราเอง
- **B**: ใช้ CUDA ทำ kernel matmul Q8_0 ตัวแรก เทียบกับ Vulkan 81.6 t/s
- **C**: เก็บสถานะนี้เป็น baseline, กลับไปทำให้ tier (GGUF-direct กับ rail unified) ก่อน

---

*รายงานนี้เขียนจาก tool output จริง ณ วันที่ 2026-08-10 — ทุกตัวเลข trace ได้จาก command ที่รัน*