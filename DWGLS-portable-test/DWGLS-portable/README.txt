DWGLS — Data Flow Management System
====================================

Quick Start:
  1. Edit config.json (set model path)
  2. Place your .gguf model in models/
  3. Run: bin\kv_flow_demo.exe config.json

Directory Structure:
  bin/         Compiled binaries
  dll/         Required DLLs (DO NOT MODIFY)
  models/      Place .gguf model files here
  config.json  Runtime configuration (edit this)

Binaries:
  kv_flow_demo.exe     KV buffer storage demo (3 flows)
  kv_rid_serve.exe     llama STATE checkpoint/restore
  geofs_rid.exe        GeoFS ↔ RID slot region
  kv_geo_addr_test.exe Geometric addressing validation
  kv_impact_test.exe   Dead slot safety analysis
  kv_container_test.exe Container format test
  kv_encode_test.exe   KV encode/decode test
  gguf_roundtrip.exe   GGUF language roundtrip

Requirements:
  - Windows 10/11 x64
  - No additional installation needed

For more info: https://github.com/M111ON/DWGLS