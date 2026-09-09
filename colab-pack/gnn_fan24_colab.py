#!/usr/bin/env python3
"""
GNN+Fan24 Tile Connectivity — Colab Training Pack
===================================================
3-Layer Pipeline: Wang hard gate → GNN+Fan24 soft rank → edge match

Upload to Colab:
  1. Upload this script + 3 data files:
     - gnn_colab_train_fan24.py  (this file)
     - smollm_all.json
     - qwen3_all.json
     - kokoro_all.json
  2. Run all cells

  Or paste into a Colab notebook cell-by-cell.

Fan24 adds 6 features per node:
  gear_sin/cos (ring-24 circular position),
  crt_kis (KIS cube wheel), crt_hyp (hyperbolic axis wheel),
  lang_id (9-language identity), dist_center (inner sanctuary distance)
"""

import subprocess, sys, os, time

def sh(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        print(f"  WARN: {cmd} → {r.stderr[:200]}")
    return r

# ═══════════════════════════════════════════════════════════════
# Cell 1: GPU info
# ═══════════════════════════════════════════════════════════════
print("=== GPU ===")
print(sh("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader").stdout.strip() or "No GPU detected")

# ═══════════════════════════════════════════════════════════════
# Cell 2: Verify data files
# ═══════════════════════════════════════════════════════════════
print("\n=== Data files ===")
missing = []
for f in ['smollm_all.json', 'qwen3_all.json', 'kokoro_all.json']:
    if os.path.exists(f):
        sz = os.path.getsize(f) / 1024
        print(f"  {f}: {sz:.0f} KB")
    else:
        missing.append(f)
        print(f"  {f}: MISSING")

if missing:
    print(f"\nUpload these files to Colab session storage:")
    for f in missing:
        print(f"  from google.colab import files; files.upload(filename='{f}')")
    print("\nThen re-run this cell.")
    # Try interactive upload
    try:
        from google.colab import files
        for f in missing:
            print(f"Uploading {f}...")
            uploaded = files.upload()
            if f in uploaded:
                print(f"  {f}: {len(uploaded[f])/1024:.0f} KB")
    except Exception as e:
        print(f"  Auto-upload failed: {e}")
        print("  Upload manually, then re-run.")

# ═══════════════════════════════════════════════════════════════
# Cell 3: Install deps
# ═══════════════════════════════════════════════════════════════
print("\n=== Install deps ===")
sh(f"{sys.executable} -m pip install -q torch")

# ═══════════════════════════════════════════════════════════════
# Cell 4: Run training
# ═══════════════════════════════════════════════════════════════
print("\n=== Training ===")
t0 = time.time()
r = sh(f"{sys.executable} gnn_colab_train_fan24.py")
print(r.stdout)
if r.stderr:
    print("STDERR:", r.stderr[-500:])
elapsed = time.time() - t0
print(f"\nTotal time: {elapsed/60:.1f} min")

# ═══════════════════════════════════════════════════════════════
# Cell 5: Export to C header
# ═══════════════════════════════════════════════════════════════
print("\n=== Export to C header ===")
if os.path.exists('gnn_fan24_best.pt') and os.path.exists('gnn_export_fan24.py'):
    r = sh(f"{sys.executable} gnn_export_fan24.py gnn_fan24_best.pt gnn_fan24_model.h")
    print(r.stdout)
    if r.stderr:
        print("STDERR:", r.stderr[-500:])
else:
    print("  Skipping export (missing .pt or export script)")

# ═══════════════════════════════════════════════════════════════
# Cell 6: Results
# ═══════════════════════════════════════════════════════════════
print("\n=== Output files ===")
for f in ['gnn_fan24_best.pt']:
    if os.path.exists(f):
        sz = os.path.getsize(f) / 1024
        print(f"  {f}: {sz:.0f} KB")
    else:
        print(f"  {f}: NOT FOUND")

print("\n=== Download model ===")
try:
    from google.colab import files
    files.download('gnn_fan24_best.pt')
except Exception as e:
    print(f"  Download failed: {e}")
    print("  Right-click gnn_fan24_best.pt in file browser to download")
