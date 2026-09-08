#!/usr/bin/env python3
"""
GNN Tile Connectivity Trainer (Fast CPU version)
=================================================
Replaces fixed quantize(edge_sum) 4-bin formula with learned heuristic.

Two-phase approach:
  Phase 1: Generate and cache all grid data (offline)
  Phase 2: Train GNN on pre-computed tensors (fast)

Usage:
  python gnn_tile_train.py --generate   # Phase 1: generate cache
  python gnn_tile_train.py --train      # Phase 2: train
  python gnn_tile_train.py              # Both phases
"""

import argparse
import json
import random
import time
from pathlib import Path

import torch
import torch.nn as nn
import torch.nn.functional as F

CACHE_DIR = Path(__file__).parent / "gnn_cache"

# ── Grid math ────────────────────────────────────────────────
LINES_3x3 = [
    [0,1,2], [3,4,5], [6,7,8],   # rows
    [0,3,6], [1,4,7], [2,5,8],   # cols
    [0,4,8], [2,4,6],             # diags
]

ADJ_3x3 = [
    [1, 3], [0, 2, 4], [1, 5],
    [0, 4, 6], [1, 3, 5, 7], [2, 4, 8],
    [3, 7], [4, 6, 8], [5, 7],
]

def line_sum_fingerprint(grid):
    return tuple(sum(grid[p] for p in line) for line in LINES_3x3)

def grid_edges(fp):
    return (fp[0], fp[5], fp[2], fp[3])  # top, right, bottom, left

# ── D4 symmetry ─────────────────────────────────────────────
def d4_rotate90(g):
    return [g[6], g[3], g[0], g[7], g[4], g[1], g[8], g[5], g[2]]

def d4_reflect(g):
    return [g[2], g[1], g[0], g[5], g[4], g[3], g[8], g[7], g[6]]

def d4_all(g):
    ors = [g]
    for _ in range(3):
        g = d4_rotate90(g)
        ors.append(g)
    g = d4_reflect(ors[0])
    ors.append(g)
    for _ in range(3):
        g = d4_rotate90(g)
        ors.append(g)
    return ors

# ── Pre-compute node features ───────────────────────────────
def node_features(grid, fp):
    """9 nodes × 6 features each."""
    x = []
    for i in range(9):
        r, c = i // 3, i % 3
        row_sum = sum(grid[r*3:r*3+3])
        col_sum = grid[c] + grid[c+3] + grid[c+6]
        x.append([grid[i]/9.0, r/2.0, c/2.0, row_sum/30.0, col_sum/30.0, fp[i%8]/30.0])
    return x  # [9, 6]

# ── Phase 1: Generate cache ─────────────────────────────────
def generate_cache(n_samples=100000, seed=42):
    CACHE_DIR.mkdir(exist_ok=True)
    rng = random.Random(seed)

    print("Generating random grids (sampling from permutations)...")
    t0 = time.time()

    # Sample random grids without materializing all 362K permutations
    N_GRIDS = min(50000, n_samples * 2)  # enough unique grids for training
    seen = set()
    grids = []
    rng2 = random.Random(seed)
    while len(grids) < N_GRIDS:
        perm = rng2.sample(range(1, 10), 9)
        key = tuple(perm)
        if key not in seen:
            seen.add(key)
            grids.append(list(perm))
    print(f"  Sampled {len(grids)} unique grids in {time.time()-t0:.1f}s")

    # Pre-compute features
    print("Pre-computing features...")
    t0 = time.time()
    nf_list = []
    ev_list = []
    for g in grids:
        fp = line_sum_fingerprint(g)
        nf = node_features(g, fp)
        ev = list(grid_edges(fp))
        nf_list.append(nf)
        ev_list.append(ev)

    feat_tensor = torch.tensor(nf_list, dtype=torch.float32)
    edge_tensor = torch.tensor(ev_list, dtype=torch.float32)
    print(f"  Feature tensor: {feat_tensor.shape}, Edge tensor: {edge_tensor.shape}")
    print(f"  Done in {time.time()-t0:.1f}s")

    torch.save(feat_tensor, CACHE_DIR / "features.pt")
    torch.save(edge_tensor, CACHE_DIR / "edges.pt")

    # Generate training pairs from the sampled grids
    print(f"Generating {n_samples} training pairs...")
    t0 = time.time()
    n_grids = len(grids)
    idx_a = torch.randint(0, n_grids, (n_samples,))
    idx_b = torch.randint(0, n_grids, (n_samples,))
    dirs = torch.randint(0, 2, (n_samples,))

    labels = []
    for i in range(n_samples):
        ia, ib = idx_a[i].item(), idx_b[i].item()
        ea = ev_list[ia]
        eb = ev_list[ib]
        d = dirs[i].item()

        if d == 0:  # TB: A.bottom -> B.top
            compat = (ea[2] == eb[0])
            frontier = eb[2]
        else:  # LR: A.right -> B.left
            compat = (ea[1] == eb[3])
            frontier = eb[1]

        if not compat:
            labels.append(0)
        elif frontier <= 2 or frontier >= 26:
            labels.append(0)
        else:
            labels.append(1)

    labels = torch.tensor(labels, dtype=torch.float32)
    pos = labels.sum().item()
    neg = len(labels) - pos
    print(f"  Positive: {int(pos)} ({pos/len(labels)*100:.1f}%)")
    print(f"  Negative: {int(neg)} ({neg/len(labels)*100:.1f}%)")
    print(f"  Done in {time.time()-t0:.1f}s")

    torch.save(idx_a, CACHE_DIR / "idx_a.pt")
    torch.save(idx_b, CACHE_DIR / "idx_b.pt")
    torch.save(dirs, CACHE_DIR / "dirs.pt")
    torch.save(labels, CACHE_DIR / "labels.pt")
    print(f"Cache saved to {CACHE_DIR}/")

# ── Model ───────────────────────────────────────────────────
class TileGNNFast(nn.Module):
    """
    Fast tile connectivity predictor.
    Uses pre-computed node features, no graph structure needed for 3x3 grid
    (all nodes are fully characterized by position + value).

    Architecture:
      - Per-tile encoder: 2-layer MLP on flattened 9x6 features → hidden
      - Global context: edge values (4 per tile)
      - Pair predictor: MLP on [repr_A, repr_B, direction]
    """
    def __init__(self, node_feat_dim=6, hidden_dim=128):
        super().__init__()
        self.tile_encoder = nn.Sequential(
            nn.Linear(9 * node_feat_dim, hidden_dim),
            nn.LayerNorm(hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.LayerNorm(hidden_dim),
            nn.ReLU(),
        )
        self.edge_encoder = nn.Linear(4, 32)

        self.predictor = nn.Sequential(
            nn.Linear(hidden_dim * 2 + 32 + 32 + 1, hidden_dim),
            nn.LayerNorm(hidden_dim),
            nn.ReLU(),
            nn.Dropout(0.1),
            nn.Linear(hidden_dim, hidden_dim // 2),
            nn.ReLU(),
            nn.Linear(hidden_dim // 2, 1),
        )

    def encode_tile(self, node_feats, edge_vals):
        """Encode one tile: node features [9,6] + edge values [4]."""
        h = self.tile_encoder(node_feats.flatten(1))  # [batch, hidden]
        e = self.edge_encoder(edge_vals)  # [batch, 32]
        return h, e

    def forward(self, nf_a, ev_a, nf_b, ev_b, dirs):
        """
        Args:
          nf_a, nf_b: [batch, 9, 6] node features
          ev_a, ev_b: [batch, 4] edge values
          dirs: [batch, 1] direction embedding (0=TB, 1=LR)
        """
        ha, ea = self.encode_tile(nf_a, ev_a)
        hb, eb = self.encode_tile(nf_b, ev_b)
        combined = torch.cat([ha, hb, ea, eb, dirs.unsqueeze(-1)], dim=-1)
        return self.predictor(combined).squeeze(-1)

# ── Phase 2: Train ──────────────────────────────────────────
def train(args):
    print("Loading cache...")
    feat = torch.load(CACHE_DIR / "features.pt")
    edges = torch.load(CACHE_DIR / "edges.pt")
    idx_a = torch.load(CACHE_DIR / "idx_a.pt")
    idx_b = torch.load(CACHE_DIR / "idx_b.pt")
    dirs = torch.load(CACHE_DIR / "dirs.pt")
    labels = torch.load(CACHE_DIR / "labels.pt")

    n = len(labels)
    print(f"Dataset: {n} pairs")

    # D4 augmentation: for each tile, pick a random orientation
    # Pre-generate random orientation indices
    orientations = torch.randint(0, 8, (2, n))  # [2, n] for tile A and B

    # Split
    train_n = int(n * 0.8)
    perm = torch.randperm(n)
    train_idx = perm[:train_n]
    val_idx = perm[train_n:]

    # Compute class weights for imbalanced data
    n_pos = labels.sum().item()
    n_neg = len(labels) - n_pos
    pos_weight = torch.tensor([n_neg / n_pos])
    print(f"Class balance: pos={int(n_pos)} neg={int(n_neg)} pos_weight={pos_weight.item():.1f}")

    model = TileGNNFast(node_feat_dim=6, hidden_dim=128)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"Model parameters: {n_params:,}")

    optimizer = torch.optim.Adam(model.parameters(), lr=3e-4, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs)

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    model = model.to(device)
    pos_weight = pos_weight.to(device)
    print(f"Device: {device}")

    def focal_loss(logits, labels, alpha=0.25, gamma=2.0):
        """Focal loss for class imbalance."""
        bce = F.binary_cross_entropy_with_logits(logits, labels, reduction='none')
        pt = torch.exp(-bce)
        focal = alpha * (1 - pt) ** gamma * bce
        return focal.mean()

    best_val = 0
    t_start = time.time()

    for epoch in range(args.epochs):
        # Mini-batch training with oversampling positives
        model.train()
        # Create balanced batch: 50% positive, 50% negative
        pos_indices = train_idx[labels[train_idx] == 1]
        neg_indices = train_idx[labels[train_idx] == 0]

        total_loss, tp, fp, fn, tn_count = 0, 0, 0, 0, 0

        n_batches = (train_n + args.batch_size - 1) // args.batch_size
        for b in range(n_batches):
            # Sample balanced batch
            half = args.batch_size // 2
            pos_sample = pos_indices[torch.randint(len(pos_indices), (half,))]
            neg_sample = neg_indices[torch.randint(len(neg_indices), (args.batch_size - half,))]
            batch_idx = torch.cat([pos_sample, neg_sample])
            batch_idx = batch_idx[torch.randperm(len(batch_idx))]

            ia = idx_a[batch_idx]
            ib = idx_b[batch_idx]
            d = dirs[batch_idx].float()
            lb = labels[batch_idx]

            nf_a = feat[ia].to(device)
            nf_b = feat[ib].to(device)
            ev_a = edges[ia].to(device)
            ev_b = edges[ib].to(device)
            d, lb = d.to(device), lb.to(device)

            logits = model(nf_a, ev_a, nf_b, ev_b, d)

            # Focal loss + pos_weight
            loss = focal_loss(logits, lb) + 0.5 * F.binary_cross_entropy_with_logits(
                logits, lb, pos_weight=pos_weight)

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            total_loss += loss.item() * lb.size(0)
            preds = (logits > 0).float()
            tp += ((preds == 1) & (lb == 1)).sum().item()
            fp += ((preds == 1) & (lb == 0)).sum().item()
            fn += ((preds == 0) & (lb == 1)).sum().item()
            tn_count += ((preds == 0) & (lb == 0)).sum().item()

        train_prec = tp / (tp + fp) if (tp + fp) > 0 else 0
        train_rec = tp / (tp + fn) if (tp + fn) > 0 else 0
        train_f1 = 2 * train_prec * train_rec / (train_prec + train_rec) if (train_prec + train_rec) > 0 else 0

        # Validate
        model.eval()
        val_tp, val_fp, val_fn, val_tn = 0, 0, 0, 0
        with torch.no_grad():
            for start in range(0, len(val_idx), args.batch_size):
                end = min(start + args.batch_size, len(val_idx))
                batch_idx = val_idx[start:end]

                ia = idx_a[batch_idx]
                ib = idx_b[batch_idx]
                d = dirs[batch_idx].float()
                lb = labels[batch_idx]

                nf_a = feat[ia].to(device)
                nf_b = feat[ib].to(device)
                ev_a = edges[ia].to(device)
                ev_b = edges[ib].to(device)
                d, lb = d.to(device), lb.to(device)

                logits = model(nf_a, ev_a, nf_b, ev_b, d)
                preds = (logits > 0).float()
                val_tp += ((preds == 1) & (lb == 1)).sum().item()
                val_fp += ((preds == 1) & (lb == 0)).sum().item()
                val_fn += ((preds == 0) & (lb == 1)).sum().item()
                val_tn += ((preds == 0) & (lb == 0)).sum().item()

        val_prec = val_tp / (val_tp + val_fp) if (val_tp + val_fp) > 0 else 0
        val_rec = val_tp / (val_tp + val_fn) if (val_tp + val_fn) > 0 else 0
        val_f1 = 2 * val_prec * val_rec / (val_prec + val_rec) if (val_prec + val_rec) > 0 else 0
        val_acc = (val_tp + val_tn) / (val_tp + val_fp + val_fn + val_tn)
        scheduler.step()

        if val_f1 > best_val:
            best_val = val_f1
            torch.save(model.state_dict(), CACHE_DIR / "gnn_best.pt")

        elapsed = time.time() - t_start
        marker = ' *' if val_f1 == best_val else ''
        if (epoch + 1) % 5 == 0 or epoch == 0:
            print(f"  Epoch {epoch+1:3d}/{args.epochs}: "
                  f"loss={total_loss/(tp+fp+fn+tn_count):.4f} "
                  f"train_f1={train_f1:.3f} val_f1={val_f1:.3f} "
                  f"val_acc={val_acc:.3f}"
                  f" [{elapsed:.0f}s]{marker}")

    print(f"\n{'='*50}")
    print(f"Best val F1: {best_val:.3f}")
    print(f"Baseline (quantize 4-bin):  0.436")
    print(f"F1 improvement: {(best_val - 0.436) * 100:+.1f}%")
    print(f"Total time: {time.time()-t_start:.0f}s")
    print(f"Model saved: {CACHE_DIR / 'gnn_best.pt'}")

# ── Main ────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generate', action='store_true', help='Generate cache only')
    parser.add_argument('--train', action='store_true', help='Train only (needs cache)')
    parser.add_argument('--samples', type=int, default=100000)
    parser.add_argument('--epochs', type=int, default=30)
    parser.add_argument('--batch-size', type=int, default=512)
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()

    random.seed(args.seed)
    torch.manual_seed(args.seed)

    if args.generate or not (CACHE_DIR / "labels.pt").exists():
        generate_cache(args.samples, args.seed)

    if args.train or not args.generate:
        train(args)

if __name__ == '__main__':
    main()
