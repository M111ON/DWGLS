#!/usr/bin/env python3
"""
GNN Tile — Train on Real Weights (Colab GPU)
=============================================
Train directly on SmolLM/Qwen3/Kokoro weight distributions.

Upload: smollm_all.json + qwen3_all.json + kokoro_all.json

  !python gnn_colab_train_real.py
"""

import subprocess, sys, re, json, random, time

def ensure_deps():
    try:
        import torch
    except ImportError:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "torch"])

ensure_deps()

import torch
import torch.nn as nn
import torch.nn.functional as F

device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
print(f"Device: {device}")
if device.type == 'cuda':
    print(f"GPU: {torch.cuda.get_device_name(0)}")

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

# ── Parse JSON ──────────────────────────────────────────────
def parse_concat(path):
    with open(path) as f:
        raw = f.read()
    return [json.loads(m) for m in re.findall(
        r'\{[^{}]*"weights"\s*:\s*\[[^\]]*\][^{}]*\}', raw)]

def fp_rank(vals):
    g = sorted(range(len(vals)), key=lambda i: -vals[i])
    grid = [0]*81
    for pos, rank in enumerate(g):
        grid[rank] = pos + 1
    return grid

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

# ── Step 1: Parse real weights ──────────────────────────────
print("\n[1/5] Parsing real model weights...")
t0 = time.time()

raw_data = {}
for name, path in [('SmolLM', 'smollm_all.json'), ('Qwen3', 'qwen3_all.json'), ('Kokoro', 'kokoro_all.json')]:
    tensors = parse_concat(path)
    tiles = []
    for obj in tensors:
        vals = obj['weights']
        if len(vals) >= 81:
            grid = fp_rank(vals[:81])[:9]
            fp = line_sum_fingerprint(grid)
            tiles.append({
                'name': obj['name'],
                'nf': node_features(grid, fp),
                'edges': grid_edges(fp),
            })
    raw_data[name] = tiles
    print(f"  {name}: {len(tiles)} tiles")

# Combine all tiles
all_tiles = []
for tiles in raw_data.values():
    all_tiles.extend(tiles)
N = len(all_tiles)
print(f"  Total tiles: {N}")
print(f"  Done in {time.time()-t0:.1f}s")

# ── Step 2: Generate training pairs from real weights ───────
print(f"\n[2/5] Generating training pairs (all pairs, both directions)...")

# Generate ALL pairs for both directions
pairs_a = []  # tile indices
pairs_b = []
pairs_dir = []
pairs_gt = []

for i in range(N):
    for j in range(N):
        if i == j:
            continue
        # TB direction: A.bottom → B.top
        ea, eb = all_tiles[i]['edges'], all_tiles[j]['edges']
        gt_tb = 1 if ea[2] == eb[0] else 0
        pairs_a.append(i)
        pairs_b.append(j)
        pairs_dir.append(0)
        pairs_gt.append(gt_tb)

        # LR direction: A.right → B.left
        gt_lr = 1 if ea[1] == eb[3] else 0
        pairs_a.append(i)
        pairs_b.append(j)
        pairs_dir.append(1)
        pairs_gt.append(gt_lr)

pairs_gt = torch.tensor(pairs_gt, dtype=torch.float32)
n_pos = int(pairs_gt.sum())
n_neg = len(pairs_gt) - n_pos
pos_weight = torch.tensor([n_neg / n_pos])

print(f"  Total pairs: {len(pairs_gt)}")
print(f"  Positive: {n_pos} ({n_pos/len(pairs_gt)*100:.2f}%)")
print(f"  Negative: {n_neg} ({n_neg/len(pairs_gt)*100:.2f}%)")
print(f"  pos_weight: {pos_weight.item():.1f}")

# Pre-compute features as tensors
nf_all = torch.tensor([t['nf'] for t in all_tiles], dtype=torch.float32)
ev_all = torch.tensor([t['edges'] for t in all_tiles], dtype=torch.float32)

pairs_a_t = torch.tensor(pairs_a, dtype=torch.long)
pairs_b_t = torch.tensor(pairs_b, dtype=torch.long)
pairs_dir_t = torch.tensor(pairs_dir, dtype=torch.float32)

# ── Step 3: Train ───────────────────────────────────────────
print(f"\n[3/5] Training GNN on real weight distribution...")
t0 = time.time()

model = TileGNN(node_feat_dim=6, hidden_dim=256).to(device)
pos_weight = pos_weight.to(device)
n_params = sum(p.numel() for p in model.parameters())
print(f"  Parameters: {n_params:,}")

EPOCHS = 60
BATCH = 4096
LR = 3e-4

optimizer = torch.optim.AdamW(model.parameters(), lr=LR, weight_decay=1e-3)
scheduler = torch.optim.lr_scheduler.OneCycleLR(optimizer, max_lr=LR, epochs=EPOCHS,
                                                  steps_per_epoch=(len(pairs_gt) // BATCH))

# Split
n_pairs = len(pairs_gt)
perm = torch.randperm(n_pairs)
train_n = int(n_pairs * 0.8)
train_idx = perm[:train_n]
val_idx = perm[train_n:]

pos_indices = train_idx[pairs_gt[train_idx] == 1]
neg_indices = train_idx[pairs_gt[train_idx] == 0]

def focal_loss(logits, labels, alpha=0.25, gamma=2.0):
    bce = F.binary_cross_entropy_with_logits(logits, labels, reduction='none')
    pt = torch.exp(-bce)
    return (alpha * (1 - pt) ** gamma * bce).mean()

best_f1 = 0
for epoch in range(EPOCHS):
    model.train()
    tp, fp, fn, tn = 0, 0, 0, 0

    n_batches = train_n // BATCH
    for _ in range(n_batches):
        # Balanced sampling
        half = BATCH // 2
        if len(pos_indices) > 0 and len(neg_indices) > 0:
            pi = pos_indices[torch.randint(len(pos_indices), (half,))]
            ni = neg_indices[torch.randint(len(neg_indices), (BATCH - half,))]
            bi = torch.cat([pi, ni])[torch.randperm(BATCH)]
        else:
            bi = perm[torch.randint(train_n, (BATCH,))]

        nf_a = nf_all[pairs_a_t[bi]].to(device)
        nf_b = nf_all[pairs_b_t[bi]].to(device)
        ev_a = ev_all[pairs_a_t[bi]].to(device)
        ev_b = ev_all[pairs_b_t[bi]].to(device)
        d = pairs_dir_t[bi].to(device)
        lb = pairs_gt[bi].to(device)

        logits = model(nf_a, ev_a, nf_b, ev_b, d)
        loss = focal_loss(logits, lb) + 0.3 * F.binary_cross_entropy_with_logits(
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
            nf_a = nf_all[pairs_a_t[bi]].to(device)
            nf_b = nf_all[pairs_b_t[bi]].to(device)
            ev_a = ev_all[pairs_a_t[bi]].to(device)
            ev_b = ev_all[pairs_b_t[bi]].to(device)
            d = pairs_dir_t[bi].to(device)
            lb = pairs_gt[bi].to(device)

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
        torch.save(model.state_dict(), "gnn_real_best.pt")

    elapsed = time.time() - t0
    mark = ' *' if v_f1 == best_f1 else ''
    if (epoch + 1) % 5 == 0 or epoch == 0:
        print(f"  Epoch {epoch+1:3d}/{EPOCHS}: "
              f"train_f1={train_f1:.3f} val_f1={v_f1:.3f} "
              f"val_prec={v_prec:.3f} val_rec={v_rec:.3f}"
              f" [{elapsed:.0f}s]{mark}")

# ── Step 4: Evaluate with ranking ───────────────────────────
print(f"\n[4/5] Evaluating with ranking metrics...")
model.load_state_dict(torch.load("gnn_real_best.pt", map_location=device))
model.eval()

BATCH = 4096

for ds_name, tiles in raw_data.items():
    n = len(tiles)
    if n < 2:
        continue

    print(f"\n  === {ds_name} ({n} tiles) ===")

    for dir_name, dir_idx in [('TB', 0), ('LR', 1)]:
        # Build all pairs
        pairs = []
        for i in range(n):
            for j in range(n):
                if i != j:
                    pairs.append((i, j))

        # Score
        all_scores = []
        with torch.no_grad():
            for start in range(0, len(pairs), BATCH):
                end = min(start + BATCH, len(pairs))
                bp = pairs[start:end]
                nf_a = torch.tensor([tiles[p[0]]['nf'] for p in bp], dtype=torch.float32)
                nf_b = torch.tensor([tiles[p[1]]['nf'] for p in bp], dtype=torch.float32)
                ev_a = torch.tensor([tiles[p[0]]['edges'] for p in bp], dtype=torch.float32)
                ev_b = torch.tensor([tiles[p[1]]['edges'] for p in bp], dtype=torch.float32)
                d = torch.full((len(bp),), dir_idx, dtype=torch.float32)
                logits = model(nf_a.to(device), ev_a.to(device),
                              nf_b.to(device), ev_b.to(device), d.to(device))
                all_scores.extend(torch.sigmoid(logits).cpu().tolist())

        # Ground truth
        gt = []
        for i, j in pairs:
            ea, eb = tiles[i]['edges'], tiles[j]['edges']
            if dir_idx == 0:
                gt.append(1 if ea[2] == eb[0] else 0)
            else:
                gt.append(1 if ea[1] == eb[3] else 0)
        gt = torch.tensor(gt)
        n_pos = gt.sum().item()

        sorted_idx = torch.argsort(torch.tensor(all_scores), descending=True)

        print(f"  {dir_name}: {len(pairs)} pairs, {int(n_pos)} exact ({n_pos/len(pairs)*100:.1f}%)")
        print(f"    {'K':<6} {'Prec@K':<10} {'Rec@K':<10} {'Found'}")
        for k in [5, 10, 20, 50]:
            if k > len(pairs):
                continue
            top_k = sorted_idx[:k]
            tp_k = gt[top_k].sum().item()
            print(f"    {k:<6} {tp_k/k:<10.3f} {tp_k/max(n_pos,1):<10.3f} {int(tp_k)}/{int(n_pos)}")

        # AP
        sorted_gt = gt[sorted_idx]
        ap = sum(sorted_gt[:k+1].sum().item() / (k+1) for k in range(len(pairs))) / max(n_pos, 1)
        print(f"    AP: {ap:.4f}")

# ── Step 5: Summary ─────────────────────────────────────────
print(f"\n[5/5] Saving model...")
torch.save(model.state_dict(), "gnn_real_best.pt")
print(f"  Saved: gnn_real_best.pt")

print(f"\n{'='*60}")
print("RESULTS: GNN trained on REAL weight distribution")
print(f"{'='*60}")
print(f"Training data: {N} real tiles from SmolLM+Qwen3+Kokoro")
print(f"Pairs: {len(pairs_gt)} ({n_pos} positive = {n_pos/len(pairs_gt)*100:.2f}%)")
print(f"Best val F1: {best_f1:.3f}")
print(f"Model saved: gnn_real_best.pt")
print(f"\nKey difference from previous attempt:")
print(f"  Previous: trained on random permutations → didn't generalize")
print(f"  This:     trained on real weight tiles → should generalize")
print(f"{'='*60}")
