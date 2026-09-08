#!/usr/bin/env python3
"""
GNN Tile — Real Weights Test (Colab GPU)
=========================================
Upload along with gnn_tile_best.pt and the 3 JSON files:
  - smollm_all.json
  - qwen3_all.json
  - kokoro_all.json

  !python gnn_colab_test_real.py
"""

import subprocess, sys, re, json

def ensure_deps():
    try:
        import torch
    except ImportError:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "torch"])

ensure_deps()

import torch
import torch.nn as nn
import torch.nn.functional as F
import time
from collections import Counter

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

# ── Parse real weight JSON ──────────────────────────────────
def parse_concat(path):
    with open(path) as f:
        raw = f.read()
    return [json.loads(m) for m in re.findall(
        r'\{[^{}]*"weights"\s*:\s*\[[^\]]*\][^{}]*\}', raw)]

def fp_rank(vals):
    """Rank-based 3×3 grid from 81 weight values."""
    g = sorted(range(len(vals)), key=lambda i: -vals[i])
    grid = [0]*81
    for pos, rank in enumerate(g):
        grid[rank] = pos + 1
    return grid

# ── Model (same as training) ────────────────────────────────
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

# ── Load model ──────────────────────────────────────────────
print("\n[1/4] Loading trained GNN model...")
model = TileGNN(node_feat_dim=6, hidden_dim=256).to(device)
model.load_state_dict(torch.load("gnn_tile_best.pt", map_location=device))
model.eval()
print(f"  Parameters: {sum(p.numel() for p in model.parameters()):,}")

# ── Parse real tensors ──────────────────────────────────────
print("\n[2/4] Parsing real model weights...")
t0 = time.time()

smollm = parse_concat('smollm_all.json')
qwen3 = parse_concat('qwen3_all.json')
kokoro = parse_concat('kokoro_all.json')

datasets = {
    'SmolLM': smollm,
    'Qwen3': qwen3,
    'Kokoro': kokoro,
}

# Build tile features for each dataset
tile_data = {}
for name, tensors in datasets.items():
    tiles = []
    for obj in tensors:
        vals = obj['weights']
        if len(vals) >= 81:
            grid = fp_rank(vals[:81])
            # Use first 9 values as the 3×3 grid (rank positions 0-8)
            grid_3x3 = grid[:9]
            fp = line_sum_fingerprint(grid_3x3)
            edges = grid_edges(fp)
            nf = node_features(grid_3x3, fp)
            tiles.append({
                'name': obj['name'],
                'grid': grid_3x3,
                'fp': fp,
                'edges': edges,
                'nf': nf,
            })
    tile_data[name] = tiles
    print(f"  {name}: {len(tiles)} tiles")

print(f"  Done in {time.time()-t0:.1f}s")

# ── Evaluate GNN on real weights ────────────────────────────
print("\n[3/4] Evaluating GNN on real weights...")
t0 = time.time()

BATCH = 4096

for ds_name, tiles in tile_data.items():
    n = len(tiles)
    if n < 2:
        continue

    print(f"\n  === {ds_name} ({n} tiles) ===")

    # Prepare all pairs for TB (bottom→top) and LR (right→left)
    for direction_name, dir_idx in [('TB (bottom→top)', 0), ('LR (right→left)', 1)]:
        # Build batched features
        all_nf_a, all_nf_b = [], []
        all_ev_a, all_ev_b = [], []
        all_dirs = []

        # Generate all sequential pairs (memory order)
        for i in range(n - 1):
            t_a = tiles[i]
            t_b = tiles[i + 1]
            all_nf_a.append(t_a['nf'])
            all_nf_b.append(t_b['nf'])
            all_ev_a.append(list(t_a['edges']))
            all_ev_b.append(list(t_b['edges']))
            all_dirs.append(dir_idx)

        nf_a = torch.tensor(all_nf_a, dtype=torch.float32)
        nf_b = torch.tensor(all_nf_b, dtype=torch.float32)
        ev_a = torch.tensor(all_ev_a, dtype=torch.float32)
        ev_b = torch.tensor(all_ev_b, dtype=torch.float32)
        dirs_t = torch.tensor(all_dirs, dtype=torch.float32)

        # GNN prediction
        gnn_preds = []
        with torch.no_grad():
            for start in range(0, len(dirs_t), BATCH):
                end = min(start + BATCH, len(dirs_t))
                la = model(nf_a[start:end].to(device),
                          ev_a[start:end].to(device),
                          nf_b[start:end].to(device),
                          ev_b[start:end].to(device),
                          dirs_t[start:end].to(device))
                gnn_preds.extend((torch.sigmoid(la) > 0.5).cpu().tolist())

        gnn_preds = torch.tensor(gnn_preds)

        # Quantize baseline
        q_preds = []
        for i in range(n - 1):
            ea = tiles[i]['edges']
            eb = tiles[i + 1]['edges']
            if dir_idx == 0:  # TB
                q_preds.append(1 if (int(ea[2]) % 4 == int(eb[0]) % 4) else 0)
            else:  # LR
                q_preds.append(1 if (int(ea[1]) % 4 == int(eb[3]) % 4) else 0)
        q_preds = torch.tensor(q_preds)

        # Ground truth: edge exact match
        gt = []
        for i in range(n - 1):
            ea = tiles[i]['edges']
            eb = tiles[i + 1]['edges']
            if dir_idx == 0:
                gt.append(1 if ea[2] == eb[0] else 0)
            else:
                gt.append(1 if ea[1] == eb[3] else 0)
        gt = torch.tensor(gt)

        n_pairs = len(gt)
        n_pos_gt = gt.sum().item()

        # GNN metrics
        gnn_tp = ((gnn_preds == 1) & (gt == 1)).sum().item()
        gnn_fp = ((gnn_preds == 1) & (gt == 0)).sum().item()
        gnn_fn = ((gnn_preds == 0) & (gt == 1)).sum().item()
        gnn_tn = ((gnn_preds == 0) & (gt == 0)).sum().item()
        gnn_p = gnn_tp / max(gnn_tp + gnn_fp, 1)
        gnn_r = gnn_tp / max(gnn_tp + gnn_fn, 1)
        gnn_f1 = 2 * gnn_p * gnn_r / max(gnn_p + gnn_r, 1e-8)

        # Quantize metrics
        q_tp = ((q_preds == 1) & (gt == 1)).sum().item()
        q_fp = ((q_preds == 1) & (gt == 0)).sum().item()
        q_fn = ((q_preds == 0) & (gt == 1)).sum().item()
        q_tn = ((q_preds == 0) & (gt == 0)).sum().item()
        q_p = q_tp / max(q_tp + q_fp, 1)
        q_r = q_tp / max(q_tp + q_fn, 1)
        q_f1 = 2 * q_p * q_r / max(q_p + q_r, 1e-8)

        print(f"  {direction_name}: {n_pairs} pairs ({int(n_pos_gt)} exact-match = {n_pos_gt/n_pairs*100:.1f}%)")
        print(f"    {'Metric':<12} {'Quantize':<12} {'GNN':<12}")
        print(f"    {'Precision':<12} {q_p:<12.3f} {gnn_p:<12.3f}")
        print(f"    {'Recall':<12} {q_r:<12.3f} {gnn_r:<12.3f}")
        print(f"    {'F1':<12} {q_f1:<12.3f} {gnn_f1:<12.3f}")

        # Bandwidth savings: GNN reject rate = how many connections it blocks
        gnn_reject = (gnn_preds == 0).sum().item()
        q_reject = (q_preds == 0).sum().item()
        print(f"    Reject rate:  {q_reject/n_pairs*100:.1f}%     {gnn_reject/n_pairs*100:.1f}%")

# ── Chain simulation ────────────────────────────────────────
print(f"\n[4/4] Chain simulation (longest path via GNN gate)...")
t0 = time.time()

for ds_name, tiles in tile_data.items():
    n = len(tiles)
    if n < 2:
        continue

    # Build GNN compatibility matrix for TB direction
    all_nf_a, all_nf_b = [], []
    all_ev_a, all_ev_b = [], []
    all_dirs = []
    for i in range(n):
        for j in range(n):
            if i == j:
                continue
            all_nf_a.append(tiles[i]['nf'])
            all_nf_b.append(tiles[j]['nf'])
            all_ev_a.append(list(tiles[i]['edges']))
            all_ev_b.append(list(tiles[j]['edges']))
            all_dirs.append(0)  # TB

    nf_a = torch.tensor(all_nf_a, dtype=torch.float32)
    nf_b = torch.tensor(all_nf_b, dtype=torch.float32)
    ev_a = torch.tensor(all_ev_a, dtype=torch.float32)
    ev_b = torch.tensor(all_ev_b, dtype=torch.float32)
    dirs_t = torch.tensor(all_dirs, dtype=torch.float32)

    compat = set()
    with torch.no_grad():
        for start in range(0, len(dirs_t), BATCH):
            end = min(start + BATCH, len(dirs_t))
            logits = model(nf_a[start:end].to(device),
                          ev_a[start:end].to(device),
                          nf_b[start:end].to(device),
                          ev_b[start:end].to(device),
                          dirs_t[start:end].to(device))
            preds = (torch.sigmoid(logits) > 0.5).cpu()
            for k, p in enumerate(preds):
                if p == 1:
                    idx = start + k
                    i = idx // (n - 1)
                    j = idx % (n - 1)
                    if j >= i:
                        j += 1
                    compat.add((i, j))

    # BFS for longest chain
    best_chain = []
    for start in range(n):
        visited = {start}
        chain = [start]
        while True:
            last = chain[-1]
            found = False
            for next_t in range(n):
                if next_t not in visited and (last, next_t) in compat:
                    chain.append(next_t)
                    visited.add(next_t)
                    found = True
                    break
            if not found:
                break
        if len(chain) > len(best_chain):
            best_chain = chain[:]

    print(f"  {ds_name}: longest GNN chain = {len(best_chain)}/{n} tiles ({len(best_chain)/n*100:.1f}%)")

print(f"\n  Done in {time.time()-t0:.1f}s")
print(f"\n{'='*60}")
print("SUMMARY: GNN gate vs Quantize gate on real model weights")
print(f"{'='*60}")
