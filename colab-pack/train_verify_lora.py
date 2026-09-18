# train_verify_lora.py -- Colab (GPU): SFT Qwen2.5-0.5B-Instruct on verify_traces.jsonl -> PEFT LoRA rank-8
# run:  pip install -q transformers peft huggingface_hub safetensors datasets
#       python train_verify_lora.py verify_traces.jsonl ./verify_lora_out
# then: python peft_to_gguf_lora.py ./verify_lora_out/adapter_model.safetensors verify_lora.gguf --arch qwen2
import json, sys, torch
from transformers import AutoTokenizer, AutoModelForCausalLM
from peft import LoraConfig, get_peft_model, prepare_model_for_kbit_training

DATA, OUT = sys.argv[1], sys.argv[2]
MODEL = "Qwen/Qwen2.5-0.5B-Instruct"
RANK, EPOCHS, BATCH, LR = 8, 3, 4, 2e-4

tok = AutoTokenizer.from_pretrained(MODEL, trust_remote_code=True)
model = AutoModelForCausalLM.from_pretrained(MODEL, torch_dtype=torch.float16,
                                             trust_remote_code=True).cuda()
cfg = LoraConfig(r=RANK, lora_alpha=16, lora_dropout=0.05, bias="none",
                 task_type="CAUSAL_LM",
                 target_modules=["q_proj", "k_proj", "v_proj", "o_proj",
                                 "gate_proj", "up_proj", "down_proj"])
model = get_peft_model(model, cfg)
model.print_trainable_parameters()

recs = [json.loads(l) for l in open(DATA, encoding="utf-8")]
def encode(rec):
    full = tok.apply_chat_template(rec["messages"], tokenize=True, add_generation_prompt=False)
    # mask everything up to end of user turn: re-encode without assistant reply
    pre = tok.apply_chat_template(rec["messages"][:2], tokenize=True, add_generation_prompt=True)
    labels = [-100] * len(pre) + full[len(pre):]
    return torch.tensor(full), torch.tensor(labels)

opt = torch.optim.AdamW(model.parameters(), lr=LR)
model.train()
step = 0
for ep in range(EPOCHS):
    tot, n = 0.0, 0
    for i in range(0, len(recs), BATCH):
        chunk = recs[i:i + BATCH]
        enc = [encode(r) for r in chunk]
        L = max(len(e[0]) for e in enc)
        ids = torch.stack([torch.cat([e[0], torch.full((L - len(e[0]),), tok.pad_token_id or tok.eos_token_id)]) for e in enc]).cuda()
        lab = torch.stack([torch.cat([e[1], torch.full((L - len(e[1]),), -100)]) for e in enc]).cuda()
        attn = (ids != (tok.pad_token_id or tok.eos_token_id)).long()
        loss = model(input_ids=ids, attention_mask=attn, labels=lab).loss
        opt.zero_grad(); loss.backward(); opt.step()
        tot += loss.item(); n += 1; step += 1
    print(f"epoch {ep + 1}/{EPOCHS} loss={tot / n:.4f}", flush=True)

model.save_pretrained(OUT)
tok.save_pretrained(OUT)
print("saved", OUT)
