#!/usr/bin/env pwsh
# Extract all 290 tensors from SmolLM (sample 81 values each)
$indices = 0..289
$model = "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"
$tool = "I:/DWGLS-native-fs/build/gguf_tool.exe"

foreach ($i in $indices) {
    & $tool tensor_raw $model $i 81
}