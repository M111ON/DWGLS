# 2026-09-10_DWGLS-native-fs

**Created:** 2026-09-10 03:24:50
**Tags:** #session #DWGLS-native-fs

---

## Session ended 2026-09-10 03:24:50
**Project:** DWGLS-native-fs

E2E inference proven: tesspack→assemble→load→generate. Qwen2.5-0.5B assembled GGUF outputs identical to original. gguf_box SKIP fix, graft OOM fix, DLL no_alloc fix all landed. Next: byte-level lossless verify + GPU pipeline.

## Session ended 2026-09-10 12:00:31
**Project:** DWGLS-native-fs

GPU Pipeline Session: (1) tesspack_assemble fix - sequential offset patching + mutable header (commit f14fca6). (2) DRamTile zero-copy 47 GB/s, compressed 63 GB/s on T4. (3) Kaggle llama.cpp CUDA build proven - 136 t/s on T4, inference working via llama-server API. (4) Windows Vulkan inference proven - 84 t/s on GTX 1050 Ti (Q4_K_M). (5) Export script colab-pack/kaggle_build_and_export.py for future builds. (6) C: drive cleaned from 0 to 6GB. Next: standalone CUDA scatter decode kernel + DRamTile integrate into llama.cpp pipeline.
