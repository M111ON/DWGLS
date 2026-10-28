# Phase 1 — Small-Model KV Baseline

Status: throughput matrix complete; no llama.cpp source changes. VRAM allocation
measurement remains open.

## Test identity

- Model: `I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf`
- Model size reported by `llama-bench`: `669763072` bytes
- Parameters: `630167424`
- llama.cpp build: `10830`, commit `465e49b9c`
- CPU: `Intel(R) Pentium(R) CPU G4400 @ 3.30GHz`
- GPU present: `NVIDIA GeForce GTX 1050 Ti`, `4096 MiB`
- Backend mode: `-ngl 0` (CPU inference; no GPU VRAM claim)
- KV type: `F16/F16`
- Batch: `2048`, ubatch: `512`, threads: `2`
- Repetitions: `1`, warmup disabled

## Receipts

| Prompt tokens | Result | Prompt throughput | Time |
|---:|---|---:|---:|
| 10,000 | PASS | 198.945 t/s | 50.2652202 s |
| 50,000 | PASS | 103.589 t/s | 482.6779118 s |
| 100,000 | PASS | 54.1362 t/s | 1847.1919553 s |

## GPU receipts (`-ngl 30`, Vulkan)

| Prompt tokens | Result | Prompt throughput | Time |
|---:|---|---:|---:|
| 10,000 | PASS | 322.915 t/s | 30.9679335 s |
| 50,000 | PASS | 129.476 t/s | 386.1707424 s |
| 100,000 | PASS | 83.0188 t/s | 1204.5470130 s |

The receipt reports Vulkan backend discovery and `n_gpu_layers: 30`.

The combined 10k/50k invocation exited with code `4` after emitting the first two
records. A separate 100k run completed. Its receipt also contains a second
`n_prompt=0, n_gen=1` generation row; that row is not part of the 100k prompt
measurement and is excluded here.

## Interpretation

This is a throughput baseline only. `llama-bench` output does not expose process
RSS or VRAM allocation, so it cannot by itself satisfy the handoff's VRAM-reduction
gate. The CPU run is useful for prompt-time comparison and context-capacity failure
provenance, but it is not evidence that KV was resident in GPU memory.

Existing cold-KV evidence remains separate:

- F16 KV: `24.0 MiB` at context `2048`
- Q8/Q8 KV: `12.75 MiB`
- K8V4 KV: `9.75 MiB`
- K8V4 passed the recorded teacher-forced gate on Qwen2.5-0.5B, but those results
  were CPU-only and short-context; they do not replace this Phase 1 baseline.

## Next measured step

After the 100k receipt, run the same model/context matrix with an explicit GPU
configuration and capture runtime GPU memory before/after, or mark GPU measurement
unsupported on this 4 GB device. Do not claim the `>50%` gate without that receipt.
