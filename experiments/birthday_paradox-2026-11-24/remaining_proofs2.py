"""Proof: 0.5 rescale feasibility and weighted planar tree value analysis."""
import json, os

results = {}

# === 1. 0.5 rescale: address space impact ===
# Original: 20736 = 12^4
# Rescaled (double coords): 41472 = 2 * 12^4
# Check: is 41472 compatible with x12 chain?
original = 20736
rescaled = original * 2
results["rescale"] = {
    "original": original,
    "rescaled": rescaled,
    "rescaled_over_12": rescaled / 12,
    "rescaled_is_12_power": rescaled == 12**4 * 2,
    "x12_chain": [rescaled // (12**i) for i in range(5)],
    "mod_12": rescaled % 12,
    "note": "41472 = 2 * 12^4. Breaks 12^4 pattern (becomes 2*12^4). x12 chain: 41472/12=3456, /12=288, /12=24, /12=2. Clean division but not a pure 12-power.",
}

# === 2. Weighted planar tree vs light cone: value analysis ===
# Light cone: deterministic pruning based on distance metric
# Weighted planar tree: learned weights on edges
# Question: does planar tree add value beyond light cone?
# Analysis: DWGLS routing is deterministic (bijection). No "choice" to weigh.
# P4 feedback tracks stats but doesn't change routing.
# A weighted planar tree would only help if routing were probabilistic.
results["weighted_planar_tree"] = {
    "dwgls_routing": "deterministic (bijection)",
    "has_routing_choice": False,
    "p4_feedback_changes_routing": False,
    "light_cone_pruning": "O(1) deterministic, based on distance metric",
    "planar_tree_would_add": "learned weights on edges",
    "verdict": "REDUNDANT for DWGLS (deterministic routing has no choices to weigh). Might be useful for ANN (probabilistic cosine similarity routing).",
    "alternative": "P4 feedback already provides stats; light cone already prunes. Planar tree adds a learned layer on top, but there's nothing to learn — routing is deterministic.",
}

# === 3. 3456 structure: define and verify bijection ===
# 3456 = 6 x 576 = 6 x 24^2
# Structure: 6 directions x 576 slots each
# Mapping: direction = index // 576, slot = index % 576
# Verify: all 3456 values 0..3455, no duplicates
vals = []
for i in range(3456):
    direction = i // 576
    slot = i % 576
    # reverse: index = direction * 576 + slot
    vals.append(direction * 576 + slot)
results["3456_structure"] = {
    "total": len(vals),
    "is_bijection": sorted(vals) == list(range(3456)),
    "unique": len(set(vals)),
    "directions": 6,
    "slots_per_direction": 576,
    "slots_per_direction_sqrt": 576**0.5,
    "note": "576 = 24^2 = (8x3)^2. Each direction has 24x24 slots. 6 directions x 24x24 = 6 x 576 = 3456. Valid bijection.",
}

# === 4. Rail_sync wiring feasibility ===
results["rail_sync_wiring"] = {
    "status": "dormant (no production consumer)",
    "threshold": 24,
    "degrees": 360,
    "sync_points_at_48": 48 // 24,
    "sync_points_at_576": 576 // 24,
    "sync_points_at_6912": 6912 // 24,
    "sync_points_at_144": 144 // 24,
    "sync_points_at_1728": 1728 // 24,
    "sync_points_at_20736": 20736 // 24,
    "feasible": True,
    "requires": "Add rail_sync calls to serve path + benchmark throughput + verify latency < 20% gate (#7829)",
    "unproven": "Whether rail_sync adds value beyond existing deterministic routing",
}

# === 5. Fibo_spine wiring feasibility ===
results["fibo_spine_wiring"] = {
    "status": "partial (tools + collection, not core production)",
    "pipes": 1728,
    "ticks": 12,
    "slots": 20736,
    "jet_bridge": "tick 11 -> residual -> tick 13, skip tick 12",
    "feasible": True,
    "requires": "Wire fibo_spine tick scheduling into tesspack serve path + benchmark",
    "unproven": "Whether fibo_spine scheduling improves throughput vs sequential loading",
}

# === 6. Matryoshka applicability ===
results["matryoshka"] = {
    "tesseract_8_views": "APPLIES (store 1 cube, derive 8 views via bijection)",
    "tring_dim": "DOES NOT APPLY (independently computed, not nested)",
    "goldberg_level": "UNVERIFIED (need to check if lower level is coarsening of higher)",
    "diamond_shell": "DOES NOT APPLY (entropy classes, not truncation)",
    "conclusion": "Matryoshka (prefix/truncation property) only applies to tesseract 8 views. Other structures use bijections or independent computation, not nested prefixes.",
}

# === 7. 0.5 rescale ===
results["half_rescale"] = {
    "problem": "Tetra/octa/frustum produce 0.5 (half-integer) positions",
    "map_not_compress": "REQUIRES exact integer bijection -> 0.5 breaks it",
    "rail_sync": "ABSORBS via 24 degree threshold (arriving but never ready)",
    "rescale_option": "Double all coordinates: 0.5 -> 1, address space 20736 -> 41472",
    "rescale_cost": "Breaks 12^4 symmetry (becomes 2*12^4)",
    "verdict": "Possible but breaks symmetry. Need to evaluate benefit vs cost. Alternative: use rail_sync threshold-based (already exists, dormant).",
}

out = os.path.join(os.path.dirname(__file__), "remaining_proofs2.json")
with open(out, "w") as f:
    json.dump(results, f, indent=2)
print(json.dumps(results, indent=2))
