# deploy_scatter_v2.ps1 — Deploy tess_scatter_bench_v2 to Colab T4
# Usage: pwsh deploy_scatter_v2.ps1 [tesspack_file]
# Requires: colab-cli authenticated, session "dwgls-gpu5"

param(
    [string]$Tesspack = "F:\model\bonsai-4b-q1_0.tesspack"
)

$S = "dwgls-gpu5"
$ErrorActionPreference = "Stop"

Write-Host "=== Scatter Bench v2 — Colab T4 Deploy ===" -ForegroundColor Cyan
Write-Host "Tesspack: $Tesspack"

# 1. Create session (if not exists)
Write-Host "`n--- Creating Colab session ---" -ForegroundColor Yellow
colab new -s $S --gpu T4 2>&1

# 2. Upload CUDA kernel
Write-Host "`n--- Uploading scatter bench kernel ---" -ForegroundColor Yellow
colab exec -s $S --timeout 30 -f /dev/stdin @"
import os
os.makedirs("/content/scatter", exist_ok=True)
print("dir ready")
"@

colab upload -s $S "I:\DWGLS-native-fs\bench\tess_scatter_bench_v2.cu" /content/scatter/

# 3. Compile on Colab
Write-Host "`n--- Compiling on Colab T4 ---" -ForegroundColor Yellow
colab exec -s $S --timeout 120 -f /dev/stdin @"
!cd /content/scatter && nvcc -O3 -std=c++17 -arch=sm_75 -o tess_scatter_v2 tess_scatter_bench_v2.cu -lm
!ls -la /content/scatter/tess_scatter_v2
"@

# 4. Upload tesspack
Write-Host "`n--- Uploading tesspack ---" -ForegroundColor Yellow
colab upload -s $S $Tesspack /content/scatter/model.tesspack

# 5. Run benchmark
Write-Host "`n--- Running GPU benchmark ---" -ForegroundColor Yellow
colab exec -s $S --timeout 600 -f /dev/stdin @"
!cd /content/scatter && ./tess_scatter_v2 model.tesspack 2>&1
"@

Write-Host "`n=== Done ===" -ForegroundColor Green
