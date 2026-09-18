# peft_to_gguf_lora.py -- PEFT adapter_model.safetensors -> GGUF LoRA (llama.cpp shape contract)
# contract (llama-adapter.cpp): A.ne=[base_ne0, r] (i.e. [in, r]), B.ne=[r, base_ne1] ([r, out])
# PEFT stores lora_A [r, in], lora_B [out, r] -> transpose both.
# run:  python peft_to_gguf_lora.py adapter_model.safetensors out.gguf --arch qwen2
import struct, sys, argparse
import torch
from safetensors import safe_open

MAP = {"q_proj": "attn_q", "k_proj": "attn_k", "v_proj": "attn_v", "o_proj": "attn_output",
       "gate_proj": "ffn_gate", "up_proj": "ffn_up", "down_proj": "ffn_down"}

def parse_key(k):
    # ...layers.N.<mod>.lora_A.weight
    p = k.split(".")
    li = p.index("layers") + 1
    layer = p[li]
    mod = p[li + 2] if p[li + 1] in ("self_attn", "mlp") else p[li + 1]
    side = "a" if "lora_A" in k else "b"
    return f"blk.{layer}.{MAP[mod]}.weight.lora_{side}"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("dst"); ap.add_argument("--arch", default="qwen2")
    a = ap.parse_args()
    tensors = {}
    with safe_open(a.src, framework="pt") as f:
        for k in f.keys():
            if "lora_" not in k or ".weight" not in k:
                continue
            t = f.get_tensor(k).float().contiguous()
            tensors[parse_key(k)] = t.t().numpy()  # transpose to GGUF contract
    assert tensors, "no lora tensors found"
    kv = [(b"general.type", b"adapter"), (b"adapter.type", b"lora"),
          (b"general.architecture", a.arch.encode())]
    out = bytearray()
    out += struct.pack("<IIQQ", 0x46554747, 3, len(tensors), len(kv))
    for k, v in kv:
        out += struct.pack("<Q", len(k)) + k + struct.pack("<I", 8)
        out += struct.pack("<Q", len(v)) + v
    off, infos = 0, []
    for name in sorted(tensors):
        t = tensors[name]
        d0, d1 = t.shape
        nb = t.nbytes
        infos.append((name.encode(), d0, d1, off))
        off += (nb + 31) & ~31
    for name, d0, d1, o in infos:
        out += struct.pack("<Q", len(name)) + name
        out += struct.pack("<I", 2) + struct.pack("<QQ", d0, d1)
        out += struct.pack("<I", 0) + struct.pack("<Q", o)  # F32
    data_start = (len(out) + 31) & ~31
    out += b"\x00" * (data_start - len(out))
    for name in sorted(tensors):
        t = tensors[name]
        out += t.tobytes()
        pad = (-len(out)) % 32
        out += b"\x00" * pad
    open(a.dst, "wb").write(bytes(out))
    print(f"wrote {a.dst}: {len(tensors)} tensors")

if __name__ == "__main__":
    main()
