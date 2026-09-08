$indices = 1..10
$model = "I:/model/Kokoro_no_espeak_Q8.gguf"
$tool = "I:/DWGLS-native-fs/build/gguf_tool.exe"

foreach ($i in $indices) {
    & $tool tensor_raw $model $i 81
}