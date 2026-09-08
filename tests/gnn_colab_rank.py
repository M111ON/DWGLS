#!/usr/bin/env python3
"""
GNN Tile — Real Weights Ranking Test (Colab GPU)
=================================================
Uses GNN as a SCORER (not binary gate) — rank connections by probability,
then take top-K. This avoids the threshold calibration problem.

Upload: gnn_tile_best.pt + smollm_all.json + qwen3_all.json + kokoro_all.json

  !python gnn_colab_rank.py
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
import time

device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
print(f"Device: {device}")

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

# ── Load model ──────────────────────────────────────────────
print("\n[1/4] Loading GNN model...")
model = TileGNN(node_feat_dim=6, hidden_dim=256).to(device)
model.load_state_dict(torch.load("gnn_tile_best.pt", map_location=device))
model.eval()
n_params = sum(p.numel() for p in model.parameters())
print(f"  Parameters: {n_params:,}")

# ── Parse real tensors ──────────────────────────────────────
print("\n[2/4] Parsing real model weights...")
t0 = time.time()

datasets = {
    'SmolLM': parse_concat('smollm_all.json'),
    'Qwen3': parse_concat('qwen3_all.json'),
    'Kokoro': parse_concat('kokoro_all.json'),
}

tile_data = {}
for name, tensors in datasets.items():
    tiles = []
    for obj in tensors:
        vals = obj['weights']
        if len(vals) >= 81:
            grid = fp_rank(vals[:81])[:9]
            fp = line_sum_fingerprint(grid)
            tiles.append({
                'name': obj['name'],
                'grid': grid,
                'fp': fp,
                'edges': grid_edges(fp),
                'nf': node_features(grid, fp),
            })
    tile_data[name] = tiles
    print(f"  {name}: {len(tiles)} tiles")

# ── Score all pairs ─────────────────────────────────────────
print(f"\n[3/4] Scoring all pairs (GNN as ranker)...")

BATCH = 4096

for ds_name, tiles in tile_data.items():
    n = len(tiles)
    if n < 2:
        continue

    print(f"\n  === {ds_name} ({n} tiles) ===")

    for dir_name, dir_idx in [('TB', 0), ('LR', 1)]:
        # Build all pairs
        pairs = []
        for i in range(n):
            for j in range(n):
                if i == j:
                    continue
                pairs.append((i, j))

        # Batch score
        all_scores = []
        with torch.no_grad():
            for start in range(0, len(pairs), BATCH):
                end = min(start + BATCH, len(pairs))
                batch_pairs = pairs[start:end]

                nf_a = torch.tensor([tiles[p[0]]['nf'] for p in batch_pairs], dtype=torch.float32)
                nf_b = torch.tensor([tiles[p[1]]['nf'] for p in batch_pairs], dtype=torch.float32)
                ev_a = torch.tensor([tiles[p[0]]['edges'] for p in batch_pairs], dtype=torch.float32)
                ev_b = torch.tensor([tiles[p[1]]['edges'] for p in batch_pairs], dtype=torch.float32)
                d = torch.full((len(batch_pairs),), dir_idx, dtype=torch.float32)

                logits = model(nf_a.to(device), ev_a.to(device),
                              nf_b.to(device), ev_b.to(device),
                              d.to(device))
                scores = torch.sigmoid(logits).cpu()
                all_scores.extend(scores.tolist())

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
        n_pairs = len(gt)

        # Sort by GNN score (descending) — best connections first
        sorted_indices = torch.argsort(torch.tensor(all_scores), descending=True)

        # Precision@K for different K values
        print(f"  {dir_name}: {n_pairs} pairs, {int(n_pos)} exact-match ({n_pos/n_pairs*100:.1f}%)")
        print(f"    {'K':<8} {'Precision@K':<15} {'Recall@K':<12} {'Found':<10}")
        print(f"    {'-'*45}")

        for k in [5, 10, 20, 50, 100]:
            if k > n_pairs:
                continue
            top_k = sorted_indices[:k]
            tp_k = gt[top_k].sum().item()
            prec_k = tp_k / k
            rec_k = tp_k / max(n_pos, 1)
            print(f"    {k:<8} {prec_k:<15.3f} {rec_k:<12.3f} {int(tp_k)}/{int(n_pos)}")

        # Average Precision
        sorted_gt = gt[sorted_indices]
        precisions = []
        for k in range(1, n_pairs + 1):
            tp_k = sorted_gt[:k].sum().item()
            precisions.append(tp_k / k)
        avg_prec = sum(precisions) / max(n_pos, 1)
        print(f"    AP (Average Precision): {avg_prec:.4f}")

        # Compare with quantize baseline
        q_scores = []
        for i, j in pairs:
            ea, eb = tiles[i]['edges'], tiles[j]['edges']
            if dir_idx == 0:
                q_scores.append(1.0 if (int(ea[2]) % 4 == int(eb[0]) % 4) else 0.0)
            else:
                q_scores.append(1.0 if (int(ea[1]) % 4 == int(eb[3]) % 4) else 0.0)

        q_sorted = torch.argsort(torch.tensor(q_scores), descending=True)
        q_tp_k = gt[q_sorted[:10]].sum().item()
        g_tp_k = gt[sorted_indices[:10]].sum().item()
        print(f"    Top-10: Quantize={int(q_tp_k)} exact, GNN={int(g_tp_k)} exact")

# ── Bandwidth savings estimate ──────────────────────────────
print(f"\n[4/4] Bandwidth savings estimate...")
print(f"  If GNN rejects bottom 50% of connections:")
print(f"  → only top 50% need data transfer")
print(f"  → ~50% bandwidth savings while keeping best connections")
print(f"\n  If GNN rejects bottom 80% of connections:")
print(f"  → only top 20% need data transfer")
print(f"  → ~80% bandwidth savings (more aggressive)")

print(f"\n{'='*60}")
print("KEY INSIGHT: GNN is a RANKER, not a binary gate")
print("Use it to sort connections by quality, not to accept/reject.")
print(f"{'='*60}")
