#!/usr/bin/env python3
"""
GNN Tile — Wang+GNN+Fan24 3-Layer Training (Colab GPU)
======================================================
Layer 1: Wang hard gate (fast reject, O(1) edge exact match)
Layer 2: GNN soft rank with Fan24 ring-24 context features
Layer 3: Edge match (ground truth confirmation)

Fan24 adds 6 features per node:
  - gear_pos_sin/cos: ring-24 circular position (sin/cos encoding)
  - crt_kis: KIS cube wheel (dc mod 8)
  - crt_hyp: hyperbolic axis wheel (dx mod 3)
  - lang_id: 9-language identity
  - dist_center: distance from inner sanctuary

Upload: smollm_all.json + qwen3_all.json + kokoro_all.json

  !python gnn_colab_train_fan24.py
"""

import subprocess, sys, re, json, random, time, math
from collections import defaultdict

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

# ── Constants ────────────────────────────────────────────────
FG_RING = 24
FG_WHEEL_KIS = 8
FG_WHEEL_HYP = 3
FG_FULL = 20736
FG_LOCAL = 144
N_LANGUAGES = 9

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

# ── Fan24 features (per node, derived from tile index) ───────
def fan24_features(tile_idx, n_tiles):
    """Compute 6 Fan24 features for a tile based on its index.

    The tile index maps to a position in the [0,20736) address space.
    Fan24 decomposes this via CRT: position → (dc mod 8, dx mod 3) on ring-24.
    """
    pos = tile_idx % FG_FULL
    gear = pos % FG_RING
    dc = gear % FG_WHEEL_KIS    # KIS cube wheel [0,7]
    dx = gear % FG_WHEEL_HYP    # hyperbolic axis wheel [0,2]
    lang = pos % N_LANGUAGES    # 9-language identity [0,8]

    # Circular encoding for ring-24 position (preserves wrap-around topology)
    angle = 2.0 * math.pi * gear / FG_RING
    gear_sin = math.sin(angle)
    gear_cos = math.cos(angle)

    # Distance from center (inner sanctuary = position 10368 = 20736/2)
    center = FG_FULL // 2
    dist = abs(pos - center) / center

    return [
        gear_sin,              # ring-24 circular position (sin)
        gear_cos,              # ring-24 circular position (cos)
        dc / (FG_WHEEL_KIS - 1),  # KIS cube wheel normalized
        dx / (FG_WHEEL_HYP - 1),  # hyperbolic axis wheel normalized
        lang / (N_LANGUAGES - 1),  # 9-language identity normalized
        dist,                  # distance from inner sanctuary
    ]

def node_features(grid, fp, tile_idx, n_tiles):
    """12 features per node: 6 original grid + 6 Fan24 context."""
    x = []
    f24 = fan24_features(tile_idx, n_tiles)
    for i in range(9):
        r, c = i // 3, i % 3
        row_sum = sum(grid[r*3:r*3+3])
        col_sum = grid[c] + grid[c+3] + grid[c+6]
        x.append([
            grid[i]/9.0, r/2.0, c/2.0,
            row_sum/30.0, col_sum/30.0, fp[i%8]/30.0,
            *f24,
        ])
    return x

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
NODE_FEAT_DIM = 12  # 6 grid + 6 Fan24

class TileGNN(nn.Module):
    def __init__(self, node_feat_dim=NODE_FEAT_DIM, hidden_dim=256):
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

# ── Parse ───────────────────────────────────────────────────
print("\n[1/6] Parsing real model weights...")
t0 = time.time()

raw_data = {}
for name, path in [('SmolLM', 'smollm_all.json'), ('Qwen3', 'qwen3_all.json'), ('Kokoro', 'kokoro_all.json')]:
    tensors = parse_concat(path)
    tiles = []
    for idx, obj in enumerate(tensors):
        vals = obj['weights']
        if len(vals) >= 81:
            grid = fp_rank(vals[:81])[:9]
            fp = line_sum_fingerprint(grid)
            tiles.append({
                'name': obj['name'],
                'nf': node_features(grid, fp, idx, len(tensors)),
                'edges': grid_edges(fp),
            })
    raw_data[name] = tiles
    print(f"  {name}: {len(tiles)} tiles")

all_tiles = []
for tiles in raw_data.values():
    all_tiles.extend(tiles)
N = len(all_tiles)
print(f"  Total: {N} tiles")
print(f"  Node feature dim: {NODE_FEAT_DIM} (6 grid + 6 Fan24)")
print(f"  Done in {time.time()-t0:.1f}s")

# ── Step 2: Wang filter + chain simulation ──────────────────
print(f"\n[2/6] Wang filter + chain simulation...")

def wang_tb_compatible(tiles, idx_a, idx_b):
    return tiles[idx_a]['edges'][2] == tiles[idx_b]['edges'][0]

def wang_lr_compatible(tiles, idx_a, idx_b):
    return tiles[idx_a]['edges'][1] == tiles[idx_b]['edges'][3]

def simulate_chain(tiles, start, direction, max_steps=5):
    if direction == 0:
        edge_idx, target_edge = 2, 0
    else:
        edge_idx, target_edge = 1, 3
    chain_len = 0
    current = start
    visited = {start}
    for step in range(max_steps):
        edge_val = tiles[current]['edges'][edge_idx]
        found = False
        for j in range(len(tiles)):
            if j not in visited and tiles[j]['edges'][target_edge] == edge_val:
                chain_len += 1
                visited.add(j)
                current = j
                found = True
                break
        if not found:
            break
    return chain_len

wang_pairs = []
for dir_idx in [0, 1]:
    compat_fn = wang_tb_compatible if dir_idx == 0 else wang_lr_compatible
    for i in range(N):
        for j in range(N):
            if i != j and compat_fn(all_tiles, i, j):
                chain_len = simulate_chain(all_tiles, j, dir_idx, max_steps=5)
                label = 1 if chain_len >= 4 else 0  # long chain = useful
                wang_pairs.append((i, j, dir_idx, chain_len, label))

n_pos = sum(1 for p in wang_pairs if p[4] == 1)
n_neg = len(wang_pairs) - n_pos
print(f"  Wang-compatible pairs: {len(wang_pairs)}")
print(f"  Positive (chain>=4): {n_pos} ({n_pos/len(wang_pairs)*100:.1f}%)")
print(f"  Negative (short chain): {n_neg} ({n_neg/len(wang_pairs)*100:.1f}%)")
print(f"  Wang reject rate: {(N*(N-1)*2 - len(wang_pairs))/(N*(N-1)*2)*100:.1f}%")

# ── Step 3: Prepare tensors ─────────────────────────────────
print(f"\n[3/6] Preparing tensors...")

nf_all = torch.tensor([t['nf'] for t in all_tiles], dtype=torch.float32)
ev_all = torch.tensor([t['edges'] for t in all_tiles], dtype=torch.float32)

wp_a = torch.tensor([p[0] for p in wang_pairs], dtype=torch.long)
wp_b = torch.tensor([p[1] for p in wang_pairs], dtype=torch.long)
wp_d = torch.tensor([p[2] for p in wang_pairs], dtype=torch.float32)
wp_labels = torch.tensor([p[4] for p in wang_pairs], dtype=torch.float32)

print(f"  Tensors ready. nf shape: {nf_all.shape}")

# ── Step 4: Train GNN on Wang survivors ─────────────────────
print(f"\n[4/6] Training GNN+Fan24 on Wang-compatible pairs...")
t0 = time.time()

model = TileGNN(node_feat_dim=NODE_FEAT_DIM, hidden_dim=256).to(device)
print(f"  Parameters: {sum(p.numel() for p in model.parameters()):,}")

EPOCHS = 60
BATCH = 1024
LR = 3e-4

optimizer = torch.optim.AdamW(model.parameters(), lr=LR, weight_decay=1e-3)
scheduler = torch.optim.lr_scheduler.OneCycleLR(optimizer, max_lr=LR, epochs=EPOCHS,
                                                  steps_per_epoch=max(1, len(wang_pairs) // BATCH))

n_wp = len(wang_pairs)
perm = torch.randperm(n_wp)
train_n = int(n_wp * 0.8)
train_idx = perm[:train_n]
val_idx = perm[train_n:]

pos_indices = train_idx[wp_labels[train_idx] == 1]
neg_indices = train_idx[wp_labels[train_idx] == 0]
print(f"  Train: {len(pos_indices)} pos, {len(neg_indices)} neg")

def focal_loss(logits, labels, alpha=0.25, gamma=2.0):
    bce = F.binary_cross_entropy_with_logits(logits, labels, reduction='none')
    pt = torch.exp(-bce)
    return (alpha * (1 - pt) ** gamma * bce).mean()

best_f1 = 0
for epoch in range(EPOCHS):
    model.train()
    tp, fp, fn, tn = 0, 0, 0, 0

    n_batches = max(1, train_n // BATCH)
    for _ in range(n_batches):
        # Natural distribution sampling (matches validation distribution)
        bi = train_idx[torch.randint(train_n, (BATCH,))]

        nf_a = nf_all[wp_a[bi]].to(device)
        nf_b = nf_all[wp_b[bi]].to(device)
        ev_a = ev_all[wp_a[bi]].to(device)
        ev_b = ev_all[wp_b[bi]].to(device)
        d = wp_d[bi].to(device)
        lb = wp_labels[bi].to(device)

        logits = model(nf_a, ev_a, nf_b, ev_b, d)
        loss = focal_loss(logits, lb)

        optimizer.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        optimizer.step()
        scheduler.step()

        preds = (torch.sigmoid(logits) > 0.5).float()
        tp += ((preds == 1) & (lb == 1)).sum().item()
        fp += ((preds == 1) & (lb == 0)).sum().item()
        fn += ((preds == 0) & (lb == 1)).sum().item()
        tn += ((preds == 0) & (lb == 0)).sum().item()

    train_prec = tp / max(tp + fp, 1)
    train_rec = tp / max(tp + fn, 1)
    train_f1 = 2 * train_prec * train_rec / max(train_prec + train_rec, 1e-8)

    model.eval()
    v_tp, v_fp, v_fn, v_tn = 0, 0, 0, 0
    with torch.no_grad():
        for start in range(0, len(val_idx), BATCH):
            bi = val_idx[start:start+BATCH]
            nf_a = nf_all[wp_a[bi]].to(device)
            nf_b = nf_all[wp_b[bi]].to(device)
            ev_a = ev_all[wp_a[bi]].to(device)
            ev_b = ev_all[wp_b[bi]].to(device)
            d = wp_d[bi].to(device)
            lb = wp_labels[bi].to(device)

            logits = model(nf_a, ev_a, nf_b, ev_b, d)
            preds = (torch.sigmoid(logits) > 0.5).float()
            v_tp += ((preds == 1) & (lb == 1)).sum().item()
            v_fp += ((preds == 1) & (lb == 0)).sum().item()
            v_fn += ((preds == 0) & (lb == 1)).sum().item()
            v_tn += ((preds == 0) & (lb == 0)).sum().item()

    v_prec = v_tp / max(v_tp + v_fp, 1)
    v_rec = v_tp / max(v_tp + v_fn, 1)
    v_f1 = 2 * v_prec * v_rec / max(v_prec + v_rec, 1e-8)

    if v_f1 > best_f1:
        best_f1 = v_f1
        torch.save(model.state_dict(), "gnn_fan24_best.pt")

    elapsed = time.time() - t0
    mark = ' *' if v_f1 == best_f1 else ''
    if (epoch + 1) % 5 == 0 or epoch == 0:
        print(f"  Epoch {epoch+1:3d}/{EPOCHS}: "
              f"train_f1={train_f1:.3f} val_f1={v_f1:.3f} "
              f"val_prec={v_prec:.3f} val_rec={v_rec:.3f}"
              f" [{elapsed:.0f}s]{mark}")

# ── Step 5: Evaluate Wang+GNN+Fan24 pipeline ────────────────
print(f"\n[5/6] Evaluating Wang+GNN+Fan24 pipeline...")
model.load_state_dict(torch.load("gnn_fan24_best.pt", map_location=device))
model.eval()

BATCH = 4096

for ds_name, tiles in raw_data.items():
    n = len(tiles)
    if n < 2:
        continue

    print(f"\n  === {ds_name} ({n} tiles) ===")

    for dir_name, dir_idx in [('TB', 0), ('LR', 1)]:
        wang_survivors = []
        for i in range(n):
            for j in range(n):
                if i == j:
                    continue
                if dir_idx == 0 and wang_tb_compatible(tiles, i, j):
                    wang_survivors.append((i, j))
                elif dir_idx == 1 and wang_lr_compatible(tiles, i, j):
                    wang_survivors.append((i, j))

        total_pairs = n * (n - 1)
        wang_rate = len(wang_survivors) / total_pairs

        print(f"  {dir_name}: Wang survivors {len(wang_survivors)}/{total_pairs} ({wang_rate*100:.1f}%)")

        if len(wang_survivors) < 2:
            continue

        all_scores = []
        with torch.no_grad():
            for start in range(0, len(wang_survivors), BATCH):
                end = min(start + BATCH, len(wang_survivors))
                bp = wang_survivors[start:end]
                nf_a = torch.tensor([tiles[p[0]]['nf'] for p in bp], dtype=torch.float32)
                nf_b = torch.tensor([tiles[p[1]]['nf'] for p in bp], dtype=torch.float32)
                ev_a = torch.tensor([tiles[p[0]]['edges'] for p in bp], dtype=torch.float32)
                ev_b = torch.tensor([tiles[p[1]]['edges'] for p in bp], dtype=torch.float32)
                d = torch.full((len(bp),), dir_idx, dtype=torch.float32)
                logits = model(nf_a.to(device), ev_a.to(device),
                              nf_b.to(device), ev_b.to(device), d.to(device))
                all_scores.extend(torch.sigmoid(logits).cpu().tolist())

        sorted_idx = torch.argsort(torch.tensor(all_scores), descending=True)

        chain_lengths = []
        for i, j in wang_survivors:
            chain_len = simulate_chain(tiles, j, dir_idx, max_steps=5)
            chain_lengths.append(chain_len)

        gnn_chain_lengths = [chain_lengths[idx] for idx in sorted_idx[:50]]
        bottom_chain_lengths = [chain_lengths[idx] for idx in sorted_idx[-50:]]
        avg_top = sum(gnn_chain_lengths) / len(gnn_chain_lengths)
        avg_bot = sum(bottom_chain_lengths) / len(bottom_chain_lengths)

        print(f"    Avg chain length (GNN top-50):    {avg_top:.2f}")
        print(f"    Avg chain length (GNN bottom-50): {avg_bot:.2f}")
        print(f"    GNN ranking improvement:          {(avg_top - avg_bot):+.2f}")

        # Fan24 perspective analysis: does GNN prefer certain gear positions?
        top50_gear = [wang_survivors[sorted_idx[k].item()][1] % FG_RING for k in range(min(50, len(sorted_idx)))]
        bot50_gear = [wang_survivors[sorted_idx[-k-1].item()][1] % FG_RING for k in range(min(50, len(sorted_idx)))]
        print(f"    Top-50 avg gear position: {sum(top50_gear)/len(top50_gear):.1f}")
        print(f"    Bot-50 avg gear position: {sum(bot50_gear)/len(bot50_gear):.1f}")

# ── Step 6: Summary ─────────────────────────────────────────
print(f"\n[6/6] Saving...")
torch.save(model.state_dict(), "gnn_fan24_best.pt")
print(f"  Saved: gnn_fan24_best.pt")

print(f"\n{'='*60}")
print("Wang+GNN+Fan24 3-Layer Pipeline Results")
print(f"{'='*60}")
print(f"Fan24 features: 6 per node (gear_sin/cos, crt_kis, crt_hyp, lang_id, dist_center)")
print(f"Node feature dim: {NODE_FEAT_DIM} (was 6, now 12)")
print(f"Parameters: {sum(p.numel() for p in model.parameters()):,}")
print(f"Wang rejects ~{100-wang_rate*100:.0f}% of pairs (O(1) exact match)")
print(f"GNN+Fan24 scores remaining ~{wang_rate*100:.0f}% (learned heuristic + multi-perspective)")
print(f"Best val F1: {best_f1:.3f}")
print(f"\nFan24 integration: tile position → ring-24 gear → CRT (dc mod 8, dx mod 3)")
print(f"  → 9 languages × multi-perspective self-repair network")
print(f"{'='*60}")
