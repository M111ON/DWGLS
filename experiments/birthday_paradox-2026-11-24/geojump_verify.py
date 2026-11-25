"""Verify geojump structural claims: 4x4 Hilbert x3 Peano stack = 48-cell block.
Maps to tring walk 720. Tests invert (sparse storage) and cross-dim combination.
"""
import numpy as np
import json, os

# ---------- geo_jump's actual implementations (from sid/geo_jump.h) ----------
def gj_hilbert(x, y, n=4):
    """_hilbert_idx from geo_jump.h: standard Hilbert on n x n."""
    d = 0
    s = n >> 1
    while s > 0:
        rx = 1 if (x & s) else 0
        ry = 1 if (y & s) else 0
        d = (d << 2) | ((3 * rx) ^ ry)
        if ry == 0:
            if rx == 1:
                x = n - 1 - x
                y = n - 1 - y
            x, y = y, x
        s >>= 1
    return d

def gj_peano(x, y, cols=4, rows=4):
    """_peano_idx from geo_jump.h: serpentine (even cols down, odd cols up)."""
    if x & 1:
        return x * rows + (rows - 1 - y)
    return x * rows + y

def is_bijection(values, n):
    """Check that values is a permutation of 0..n-1."""
    return sorted(values) == list(range(n))

results = {}

# === 1. Verify 4x4 Hilbert is bijection (16 values) ===
h_vals = [gj_hilbert(x, y, 4) for y in range(4) for x in range(4)]
results["hilbert_4x4"] = {
    "values": h_vals,
    "is_bijection": is_bijection(h_vals, 16),
    "min": min(h_vals), "max": max(h_vals),
    "unique": len(set(h_vals)),
}

# === 2. Verify 4x4 Peano (serpentine) is bijection (16 values) ===
p_vals = [gj_peano(x, y, 4, 4) for y in range(4) for x in range(4)]
results["peano_4x4"] = {
    "values": p_vals,
    "is_bijection": is_bijection(p_vals, 16),
    "min": min(p_vals), "max": max(p_vals),
    "unique": len(set(p_vals)),
}

# === 3. Verify 4x4 Hilbert x3 Peano stack = 48-cell block ===
# Block = floor 0,1,2. Each floor = 4x4 Hilbert cells. Floor index via Peano (serpentine on floors).
# In geo_jump: offset = (floor-1)*16 + hilbert_cell. floors 1..3.
block_cells = []
for floor in range(1, 4):  # floors 1,2,3
    for y in range(4):
        for x in range(4):
            cell = gj_hilbert(x, y, 4)
            offset = (floor - 1) * 16 + cell
            block_cells.append(offset)
results["block_48"] = {
    "total": len(block_cells),
    "is_bijection_0_47": is_bijection(block_cells, 48),
    "unique": len(set(block_cells)),
}

# === 4. Verify 3-block tower = 144 ===
# Tower = 3 blocks. Each block is 48 cells. Tower offset = block_index * 48 + block_cell.
tower_cells = []
for blk in range(3):
    for floor in range(1, 4):
        for y in range(4):
            for x in range(4):
                cell = gj_hilbert(x, y, 4)
                offset = (floor - 1) * 16 + cell
                tower_cells.append(blk * 48 + offset)
results["tower_144"] = {
    "total": len(tower_cells),
    "is_bijection_0_143": is_bijection(tower_cells, 144),
    "unique": len(set(tower_cells)),
}

# === 5. Tring walk 720 vs geojump 48 ===
results["tring_vs_geojump"] = {
    "tring_cycle": 720,
    "geojump_block": 48,
    "ratio_720_over_48": 720 / 48,
    "ratio_720_over_16": 720 / 16,
    "ratio_720_over_3": 720 / 3,
    "tring_spokes": 6,
    "tring_spoke_size": 120,
    "geojump_floors": 3,
    "geojump_cells_per_floor": 16,
    "geojump_blocks_per_tower": 3,
    "geojump_towers": 144,
    "note": "720 = 15 x 48 = 5 x 144 = 6 x 120. 15 = 3 x 5 (floors x ?). No clean single-factor mapping to 48.",
}

# === 6. Cross-dim combination: Hilbert 4x4 x Peano 4x4 (same size) ===
# Test: can we combine two different walk patterns on the SAME 4x4 grid?
# Method A: XOR of Hilbert and Peano indices
xor_vals = [gj_hilbert(x, y, 4) ^ gj_peano(x, y, 4, 4) for y in range(4) for x in range(4)]
# Method B: interleave (Hilbert 4 bits | Peano 4 bits)
interleave_vals = [gj_hilbert(x, y, 4) | (gj_peano(x, y, 4, 4) << 4) for y in range(4) for x in range(4)]
results["cross_dim_4x4"] = {
    "xor_unique": len(set(xor_vals)),
    "xor_collisions": 16 - len(set(xor_vals)),
    "interleave_unique": len(set(interleave_vals)),
    "interleave_collisions": 16 - len(set(interleave_vals)),
    "interleave_is_bijection": is_bijection(interleave_vals, 256),
}

# === 7. Invert (sparse storage) on 48-cell block ===
# Simulate sparse field: store inverse for occupied cells only
for occ_frac in (0.1, 0.25, 0.5):
    n_occ = max(1, int(48 * occ_frac))
    rng = np.random.default_rng(42)
    occ = rng.choice(48, size=n_occ, replace=False)
    # sparse: 2 bytes per occupied cell (index + position)
    sparse_b = n_occ * 2
    full_b = 48 * 1  # 1 byte per cell (48 fits in byte)
    results.setdefault("sparse_block", {})[f"occ_{int(occ_frac*100)}pct"] = {
        "occupied": n_occ,
        "sparse_bytes": sparse_b,
        "full_bytes": full_b,
        "savings_pct": round((1 - sparse_b / full_b) * 100, 1),
    }

# === 8. Seam detection: Hilbert neighbors vs Peano neighbors on 4x4 ===
seams = 0
total = 0
for y in range(4):
    for x in range(4):
        if x + 1 < 4:
            h1 = gj_hilbert(x, y, 4)
            h2 = gj_hilbert(x + 1, y, 4)
            p1 = gj_peano(x, y, 4, 4)
            p2 = gj_peano(x + 1, y, 4, 4)
            total += 1
            if abs(p1 - p2) > 2:  # Peano disagrees on locality
                seams += 1
results["seams_4x4"] = {
    "adjacent_pairs": total,
    "seams (peano |diff|>2)": seams,
    "seam_rate": round(seams / total, 4),
}

# === 9. Invert Hilbert x3 (user's variation: invert direction, stack 3) ===
# Invert Hilbert = reverse the curve (index -> 15 - index for 4x4)
# Stack 3 inverted Hilberts
inv_h_stack = []
for layer in range(3):
    for y in range(4):
        for x in range(4):
            inv_idx = 15 - gj_hilbert(x, y, 4)  # inverted
            inv_h_stack.append(layer * 16 + inv_idx)
results["invert_hilbert_x3"] = {
    "total": len(inv_h_stack),
    "is_bijection_0_47": is_bijection(inv_h_stack, 48),
    "unique": len(set(inv_h_stack)),
}

# === 10. Peano weave (user's variation: weave two Peano patterns) ===
# Weave: interleave two serpentine patterns (even/odd rows different direction)
weave_vals = []
for y in range(4):
    for x in range(4):
        # Pattern A: standard serpentine
        a = gj_peano(x, y, 4, 4)
        # Pattern B: inverted serpentine (flip both axes)
        b = gj_peano(3 - x, 3 - y, 4, 4)
        weave_vals.append(a ^ b)  # XOR of two Peano patterns
results["peano_weave"] = {
    "total": len(weave_vals),
    "unique": len(set(weave_vals)),
    "collisions": len(weave_vals) - len(set(weave_vals)),
}

out = os.path.join(os.path.dirname(__file__), "geojump_verify_results.json")
with open(out, "w") as f:
    json.dump(results, f, indent=2)
print(json.dumps(results, indent=2))
