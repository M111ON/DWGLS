# DWGLS Tesspack Pipeline — Colab Deploy

## Quick Start (Colab T4)

### Option A: Upload .py script
1. Open Colab, select T4 GPU runtime
2. Upload `deploy_tesspack_colab.py`
3. Upload source files: `tools/*.c` + `core/*.h` (or clone the repo)
4. Run all cells

### Option B: Use colab CLI
```bash
cd I:\DWGLS-native-fs
bash colab-pack/deploy_tesspack.sh
```

## What It Does

| Step | Tool | Output |
|------|------|--------|
| 1. Bake | tess_bake | .tess capo files from GGUF |
| 2. Pack | tess_packer | Single .tesspack file |
| 3. Assemble | tess_assemble | Reconstructed GGUF (lossless verify) |
| 4. Bench | 4 bench tools | Speed/overhead measurements |

## Model

Default: LFM2-1.2B-RAG-Q4_K_M (731 MB) — fast, fits easily on T4 (16 GB VRAM).

Change model via:
```bash
bash deploy_tesspack.sh "https://huggingface.co/.../model.gguf"
```

## Files

- `deploy_tesspack.sh` — colab CLI deploy (requires `colab` tool)
- `deploy_tesspack_colab.py` — standalone Colab notebook script
