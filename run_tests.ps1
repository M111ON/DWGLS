param([string]$Mode = "tier1")

$TIER1 = @(
    "geo_cube_in_dodeca_test","kis_4d_explore","kis_alternating_verify",
    "kis_codec_v6_standalone_test","kis_adaptive_deploy","kis_container_place",
    "section4_seal_residual","test_cell_classify","test_cube_addr","test_cube_container",
    "test_cube_in_dodeca","test_geo_diamond_map","kis_birds_eye","kis_multi_container",
    "kis_scale_test","test_geo_inference","test_geo_sid_loader","test_geo_prune",
    "test_geo_fs","test_geo_fs_mdim","test_monitor","test_phi_microscope",
    "test_safetensors_reader","test_tess_index_frame","test_tess_scale_log",
    "test_tess_frame_seek","test_tess_magnify","test_tess_hex_delta",
    "test_tess_wiring","test_bfs_persist","test_bfs_stability","test_geo_bfs_hub",
    "test_bfs_seek_anchor","test_bfs_breath"
)

if ($Mode -eq "all") {
    $Tests = Get-ChildItem tests -Filter "*.c" | ForEach-Object { $_.BaseName }
} else {
    $Tests = $TIER1
}

$pass = 0; $fail = 0; $build_fail = 0

New-Item -ItemType Directory -Force -Path build | Out-Null

foreach ($t in $Tests) {
    $src = "tests\$t.c"
    $exe = "build\${t}.exe"
    $errfile = "build\${t}.err"
    $null = & gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -I. -Icore -Icore/infra -Icore/infra -o $exe $src -lm 2>$errfile
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  BUILD FAIL  $t" -ForegroundColor Red
        Get-Content $errfile | Select-Object -First 3 | ForEach-Object { Write-Host "    $_" }
        $build_fail++
        continue
    }
    $out = & $exe 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  OK   $t" -ForegroundColor Green
        $pass++
    } else {
        Write-Host "  RUN FAIL  $t" -ForegroundColor Yellow
        $out | Select-Object -Last 3 | ForEach-Object { Write-Host "    $_" }
        $fail++
    }
}

Write-Host ""
Write-Host "=========="
Write-Host "PASS: $pass  RUN_FAIL: $fail  BUILD_FAIL: $build_fail  TOTAL: $($Tests.Count)"
