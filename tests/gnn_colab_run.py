#!/usr/bin/env python3
"""
GNN Tile Connectivity — Colab GPU Training
============================================
Upload this file to Google Colab, then:
  !pip install torch-geometric
  !python gnn_colab_run.py

Or paste into a Colab cell directly.
"""

import subprocess, sys

# Auto-install dependencies
def ensure_deps():
    try:
        import torch
    except ImportError:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "torch"])
    try:
        import torch_geometric
    except ImportError:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "torch-geometric"])

ensure_deps()

import itertools
import random
import time

import torch
import torch.nn as nn
import torch.nn.functional as F

# ── Check GPU ───────────────────────────────────────────────
device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
print(f"Device: {device}")
if device.type == 'cuda':
    print(f"GPU: {torch.cuda.get_device_name(0)}")
    print(f"VRAM: {torch.cuda.get_device_properties(0).total_mem / 1e9:.1f} GB")
else:
    print("WARNING: No GPU found. Training will be slow.")

# ── Grid math ────────────────────────────────────────────────
LINES_3x3 = [
    [0,1,2], [3,4,5], [6,7,8],
    [0,3,6], [1,4,7], [2,5,8],
    [0,4,8], [2,4,6],
]

def line_sum_fingerprint(grid):
    return tuple(sum(grid[p] for p in line) for line in LINES_3x3)

def grid_edges(fp):
    return (fp[0], fp[5], fp[2], fp[3])

def node_features(grid, fp):
    x = []
    for i in range(9):
        r, c = i // 3, i % 3
        row_sum = sum(grid[r*3:r*3+3])
        col_sum = grid[c] + grid[c+3] + grid[c+6]
        x.append([grid[i]/9.0, r/2.0, c/2.0, row_sum/30.0, col_sum/30.0, fp[i%8]/30.0])
    return x

# ── Generate data ───────────────────────────────────────────
print("\n[1/4] Generating training data...")
t0 = time.time()

N_GRIDS = 80000
N_PAIRS = 200000
SEED = 42
rng = random.Random(SEED)

# Sample unique grids
seen = set()
grids = []
while len(grids) < N_GRIDS:
    perm = rng.sample(range(1, 10), 9)
    key = tuple(perm)
    if key not in seen:
        seen.add(key)
        grids.append(list(perm))

# Pre-compute features
nf_list = []
ev_list = []
for g in grids:
    fp = line_sum_fingerprint(g)
    nf_list.append(node_features(g, fp))
    ev_list.append(list(grid_edges(fp)))

feat_tensor = torch.tensor(nf_list, dtype=torch.float32)  # [N_GRIDS, 9, 6]
edge_tensor = torch.tensor(ev_list, dtype=torch.float32)  # [N_GRIDS, 4]

# Generate pairs
idx_a = torch.randint(0, N_GRIDS, (N_PAIRS,))
idx_b = torch.randint(0, N_GRIDS, (N_PAIRS,))
dirs = torch.randint(0, 2, (N_PAIRS,))

labels = []
for i in range(N_PAIRS):
    ia, ib = idx_a[i].item(), idx_b[i].item()
    ea, eb = ev_list[ia], ev_list[ib]
    d = dirs[i].item()
    if d == 0:
        compat = (ea[2] == eb[0])
        frontier = eb[2]
    else:
        compat = (ea[1] == eb[3])
        frontier = eb[1]
    if not compat or frontier <= 2 or frontier >= 26:
        labels.append(0)
    else:
        labels.append(1)

labels = torch.tensor(labels, dtype=torch.float32)
n_pos = int(labels.sum())
n_neg = len(labels) - n_pos
pos_weight = torch.tensor([n_neg / n_pos])

print(f"  Grids: {N_GRIDS}, Pairs: {N_PAIRS}")
print(f"  Positive: {n_pos} ({n_pos/N_PAIRS*100:.1f}%)")
print(f"  Negative: {n_neg} ({n_neg/N_PAIRS*100:.1f}%)")
print(f"  Done in {time.time()-t0:.1f}s")

# ── Model ───────────────────────────────────────────────────
class TileGNN(nn.Module):
    def __init__(self, node_feat_dim=6, hidden_dim=256):
        super().__init__()
        self.tile_encoder = nn.Sequential(
            nn.Linear(9 * node_feat_dim, hidden_dim),
            nn.LayerNorm(hidden_dim),
            nn.GELU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.LayerNorm(hidden_dim),
            nn.GELU(),
        )
        self.edge_encoder = nn.Linear(4, 64)
        self.predictor = nn.Sequential(
            nn.Linear(hidden_dim * 2 + 128 + 1, hidden_dim),
            nn.LayerNorm(hidden_dim),
            nn.GELU(),
            nn.Dropout(0.15),
            nn.Linear(hidden_dim, hidden_dim // 2),
            nn.GELU(),
            nn.Linear(hidden_dim // 2, 1),
        )

    def encode_tile(self, nf, ev):
        h = self.tile_encoder(nf.flatten(1))
        e = self.edge_encoder(ev)
        return h, e

    def forward(self, nf_a, ev_a, nf_b, ev_b, d):
        ha, ea = self.encode_tile(nf_a, ev_a)
        hb, eb = self.encode_tile(nf_b, ev_b)
        combined = torch.cat([ha, hb, ea, eb, d.unsqueeze(-1)], dim=-1)
        return self.predictor(combined).squeeze(-1)

# ── Train ───────────────────────────────────────────────────
print("\n[2/4] Training GNN...")
t0 = time.time()

model = TileGNN(node_feat_dim=6, hidden_dim=256).to(device)
pos_weight = pos_weight.to(device)
n_params = sum(p.numel() for p in model.parameters())
print(f"  Parameters: {n_params:,}")

EPOCHS = 40
BATCH = 2048
LR = 5e-4

optimizer = torch.optim.AdamW(model.parameters(), lr=LR, weight_decay=1e-3)
scheduler = torch.optim.lr_scheduler.OneCycleLR(optimizer, max_lr=LR, epochs=EPOCHS,
                                                  steps_per_epoch=(N_PAIRS // BATCH))

# Split
perm = torch.randperm(N_PAIRS)
train_n = int(N_PAIRS * 0.8)
train_idx = perm[:train_n]
val_idx = perm[train_n:]

pos_indices = train_idx[labels[train_idx] == 1]
neg_indices = train_idx[labels[train_idx] == 0]

def focal_loss(logits, labels, alpha=0.25, gamma=2.0):
    bce = F.binary_cross_entropy_with_logits(logits, labels, reduction='none')
    pt = torch.exp(-bce)
    return (alpha * (1 - pt) ** gamma * bce).mean()

best_f1 = 0
for epoch in range(EPOCHS):
    model.train()
    tp, fp, fn, tn = 0, 0, 0, 0

    # Balanced batches
    n_batches = train_n // BATCH
    for _ in range(n_batches):
        half = BATCH // 2
        pi = pos_indices[torch.randint(len(pos_indices), (half,))]
        ni = neg_indices[torch.randint(len(neg_indices), (BATCH - half,))]
        bi = torch.cat([pi, ni])[torch.randperm(BATCH)]

        nf_a = feat_tensor[idx_a[bi]].to(device)
        nf_b = feat_tensor[idx_b[bi]].to(device)
        ev_a = edge_tensor[idx_a[bi]].to(device)
        ev_b = edge_tensor[idx_b[bi]].to(device)
        d = dirs[bi].float().to(device)
        lb = labels[bi].to(device)

        logits = model(nf_a, ev_a, nf_b, ev_b, d)
        loss = focal_loss(logits, lb) + 0.5 * F.binary_cross_entropy_with_logits(
            logits, lb, pos_weight=pos_weight)

        optimizer.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        optimizer.step()
        scheduler.step()

        preds = (logits > 0).float()
        tp += ((preds == 1) & (lb == 1)).sum().item()
        fp += ((preds == 1) & (lb == 0)).sum().item()
        fn += ((preds == 0) & (lb == 1)).sum().item()
        tn += ((preds == 0) & (lb == 0)).sum().item()

    train_prec = tp / max(tp + fp, 1)
    train_rec = tp / max(tp + fn, 1)
    train_f1 = 2 * train_prec * train_rec / max(train_prec + train_rec, 1e-8)

    # Validate
    model.eval()
    v_tp, v_fp, v_fn, v_tn = 0, 0, 0, 0
    with torch.no_grad():
        for start in range(0, len(val_idx), BATCH):
            bi = val_idx[start:start+BATCH]
            nf_a = feat_tensor[idx_a[bi]].to(device)
            nf_b = feat_tensor[idx_b[bi]].to(device)
            ev_a = edge_tensor[idx_a[bi]].to(device)
            ev_b = edge_tensor[idx_b[bi]].to(device)
            d = dirs[bi].float().to(device)
            lb = labels[bi].to(device)

            logits = model(nf_a, ev_a, nf_b, ev_b, d)
            preds = (logits > 0).float()
            v_tp += ((preds == 1) & (lb == 1)).sum().item()
            v_fp += ((preds == 1) & (lb == 0)).sum().item()
            v_fn += ((preds == 0) & (lb == 1)).sum().item()
            v_tn += ((preds == 0) & (lb == 0)).sum().item()

    v_prec = v_tp / max(v_tp + v_fp, 1)
    v_rec = v_tp / max(v_tp + v_fn, 1)
    v_f1 = 2 * v_prec * v_rec / max(v_prec + v_rec, 1e-8)

    if v_f1 > best_f1:
        best_f1 = v_f1
        torch.save(model.state_dict(), "gnn_tile_best.pt")

    elapsed = time.time() - t0
    mark = ' *' if v_f1 == best_f1 else ''
    if (epoch + 1) % 5 == 0 or epoch == 0:
        print(f"  Epoch {epoch+1:3d}/{EPOCHS}: "
              f"train_f1={train_f1:.3f} val_f1={v_f1:.3f} "
              f"val_prec={v_prec:.3f} val_rec={v_rec:.3f}"
              f" [{elapsed:.0f}s]{mark}")

# ── Final eval ──────────────────────────────────────────────
print(f"\n[3/4] Final evaluation...")
model.load_state_dict(torch.load("gnn_tile_best.pt"))
model.eval()

all_logits = []
with torch.no_grad():
    for start in range(0, N_PAIRS, BATCH):
        end = min(start + BATCH, N_PAIRS)
        nf_a = feat_tensor[idx_a[start:end]].to(device)
        nf_b = feat_tensor[idx_b[start:end]].to(device)
        ev_a = edge_tensor[idx_a[start:end]].to(device)
        ev_b = edge_tensor[idx_b[start:end]].to(device)
        d = dirs[start:end].float().to(device)
        logits = model(nf_a, ev_a, nf_b, ev_b, d)
        all_logits.append(logits.cpu())

all_logits = torch.cat(all_logits)
all_preds = (all_logits > 0).float()

tp = ((all_preds == 1) & (labels == 1)).sum().item()
fp = ((all_preds == 1) & (labels == 0)).sum().item()
fn = ((all_preds == 0) & (labels == 1)).sum().item()
tn = ((all_preds == 0) & (labels == 0)).sum().item()

gnn_prec = tp / max(tp + fp, 1)
gnn_rec = tp / max(tp + fn, 1)
gnn_f1 = 2 * gnn_prec * gnn_rec / max(gnn_prec + gnn_rec, 1e-8)

# Quantize baseline
q_tp, q_fp, q_fn = 0, 0, 0
for i in range(N_PAIRS):
    ia, ib = idx_a[i].item(), idx_b[i].item()
    ea, eb = edge_tensor[ia], edge_tensor[ib]
    d_val = dirs[i].item()
    if d_val == 0:
        q_match = (int(ea[2]) % 4 == int(eb[0]) % 4)
    else:
        q_match = (int(ea[1]) % 4 == int(eb[3]) % 4)
    if q_match:
        if labels[i] == 1: q_tp += 1
        else: q_fp += 1
    else:
        if labels[i] == 1: q_fn += 1

q_prec = q_tp / max(q_tp + q_fp, 1)
q_rec = q_tp / max(q_tp + q_fn, 1)
q_f1 = 2 * q_prec * q_rec / max(q_prec + q_rec, 1e-8)

print(f"\n{'='*60}")
print(f"RESULTS SUMMARY")
print(f"{'='*60}")
print(f"{'Metric':<20} {'Quantize 4-bin':<20} {'GNN (Learned)':<20}")
print(f"{'-'*60}")
print(f"{'Precision':<20} {q_prec:<20.3f} {gnn_prec:<20.3f}")
print(f"{'Recall':<20} {q_rec:<20.3f} {gnn_rec:<20.3f}")
print(f"{'F1 Score':<20} {q_f1:<20.3f} {gnn_f1:<20.3f}")
print(f"{'Accuracy':<20} {(q_tp+tn)/(N_PAIRS):<20.3f} {(tp+tn)/(N_PAIRS):<20.3f}")
print(f"{'-'*60}")
print(f"F1 improvement: {(gnn_f1 - q_f1)*100:+.1f}%")
print(f"{'='*60}")

# ── Export for C integration ────────────────────────────────
print(f"\n[4/4] Exporting model weights for C integration...")
state = model.state_dict()
export = {k: v.cpu().tolist() for k, v in state.items()}
torch.save(export, "gnn_tile_weights.pt")
print(f"  Saved: gnn_tile_weights.pt")
print(f"  Saved: gnn_tile_best.pt")
print(f"\nTotal time: {time.time()-t0:.0f}s")
