#!/usr/bin/env pwsh
# Extract all 310 tensors from Qwen3-0.6B (sample 81 values each)
$indices = 0..309
$model = "I:/model/Qwen3-0.6B-Q8_0.gguf"
$tool = "I:/DWGLS-native-fs/build/gguf_tool.exe"

foreach ($i in $indices) {
    & $tool tensor_raw $model $i 81
}