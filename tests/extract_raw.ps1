$indices = 0..9
$model = "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"
$tool = "I:/DWGLS-native-fs/build/gguf_tool.exe"

foreach ($i in $indices) {
    & $tool tensor_raw $model $i 81
}