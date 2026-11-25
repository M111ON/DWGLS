import numpy as np
import math
import random

# Simulation 1: Virtual CPU Compute Budget Limit Scope (God's Cycle)
# Compare Standard Linear/Random Search vs Bidirectional Meet-in-the-Middle across n partitions of 720
TOTAL_STATES = 720
divisors = [d for d in range(1, TOTAL_STATES + 1) if TOTAL_STATES % d == 0]

print("--- EXPERIMENT 1: VIRTUAL CPU GOD'S CYCLE BUDGET ---")
results_exp1 = []
random.seed(42)
trials = 1000

for n in [1, 2, 6, 12, 24, 30, 60, 120]:
    S = TOTAL_STATES // n
    # Theoretical 50% and 95% bounds for bidirectional meet-in-the-middle
    # In bidirectional search of space S, each side takes k steps -> k^2 ~ S -> k ~ sqrt(S)
    # Specifically, P(collision) = 1 - exp(-k^2 / S) -> for 50%, k = sqrt(S * ln 2) = 0.8325 * sqrt(S), total steps = 2*k = 1.665 * sqrt(S)
    # For single-sided birthday collision (finding duplicate/match in stream), k = 1.1774 * sqrt(S)
    
    # Empirical simulation of Meet-in-the-Middle inside partition size S (with bank routing cost)
    steps_taken = []
    for _ in range(trials):
        target = random.randrange(S)
        start = random.randrange(S)
        if start == target:
            steps_taken.append(0)
            continue
        fwd_visited = {start}
        bwd_visited = {target}
        s = 0
        while True:
            s += 1
            # step forward
            f_node = random.randrange(S)
            fwd_visited.add(f_node)
            if f_node in bwd_visited:
                steps_taken.append(2 * s - 1)
                break
            # step backward
            b_node = random.randrange(S)
            bwd_visited.add(b_node)
            if b_node in fwd_visited:
                steps_taken.append(2 * s)
                break
                
    p50 = np.percentile(steps_taken, 50)
    p95 = np.percentile(steps_taken, 95)
    p99 = np.percentile(steps_taken, 99)
    max_s = np.max(steps_taken)
    results_exp1.append((n, S, p50, p95, p99, max_s))
    print(f"n={n:3d} | S={S:3d} | P50={p50:5.1f} | P95={p95:5.1f} | P99={p99:5.1f} | Max={max_s:3d}")

# Simulation 2: Minkowski Spacetime Data Layer [-C-C-C-C-] & 24-30 Crossover
# Let x = Dimension Floor offset (e.g., -4 to +4 layers around core)
# Let ct = Cycle budget spent
# Invariant s^2 = (ct)^2 - (alpha * x)^2
# Inside Light Cone (s^2 >= 0): Causal / Deterministic access within God's Cycle
# Outside Light Cone (s^2 < 0): Spacelike / Chaos / Residual Area (Isolated)

print("\n--- EXPERIMENT 2: MINKOWSKI DATA LAYER [-C-C-C-C-] ACCESS & RESIDUAL ISOLATION ---")
# Generate 10,000 synthetic data packets with (dimension_layer x in [-4, 4], entropy/residual score)
np.random.seed(42)
num_packets = 10000
# Core ordered data has small |x| or low entropy; chaotic/residual data has high |x| (far layers) or requires high step count
x_layers = np.random.choice([-4, -3, -2, -1, 0, 1, 2, 3, 4], size=num_packets, p=[0.05, 0.08, 0.12, 0.15, 0.20, 0.15, 0.12, 0.08, 0.05])
# Required cycles to resolve without light-cone pruning: base cycles + chaos noise
required_cycles = np.abs(x_layers) * 1.2 + np.random.exponential(scale=1.5, size=num_packets)

# Set Light Cone Speed Limit c_v = 1.0 (1 cycle per dimension floor C) and max God's Cycle Budget = 4
# Condition for Timelike (Deterministic Core): ct <= God_Budget AND |x| <= ct (i.e., required_cycles <= 4 and required_cycles >= |x|? Wait: in Minkowski, Timelike means ct > |x|, meaning time available > distance to travel!)
# Let's check exact physics:
# To reach dimension floor |x| within available cycle budget B:
# You MUST have B >= |x| (Timelike: signal can reach layer x within budget B).
# If |x| > B (Spacelike: distance in dimension floors exceeds light-cone reach), it is physically unreachable within budget B -> Residual/Chaos Vault!
# Furthermore, if resolution cost (required_cycles) > B -> Cycle Overflow -> Residual/Chaos Vault!

GOD_BUDGET = 3 # At n=24..30, local half-steps ~ 3-4 cycles
timelike_mask = (np.abs(x_layers) <= GOD_BUDGET) & (required_cycles <= GOD_BUDGET)
spacelike_unreachable_mask = np.abs(x_layers) > GOD_BUDGET
chaos_overflow_mask = (np.abs(x_layers) <= GOD_BUDGET) & (required_cycles > GOD_BUDGET)

print(f"Total Packets: {num_packets}")
print(f"1. Timelike Core (Deterministic Hit <= {GOD_BUDGET} cycles): {np.sum(timelike_mask)} ({np.mean(timelike_mask)*100:.2f}%) | Avg Cycles: {np.mean(required_cycles[timelike_mask]):.2f}")
print(f"2. Spacelike Outside Cone (|x| > {GOD_BUDGET}, Instant O(1) Prune to Residual): {np.sum(spacelike_unreachable_mask)} ({np.mean(spacelike_unreachable_mask)*100:.2f}%) | Cycles Wasted: 0.00")
print(f"3. Chaos/Residual Overflow (Inside cone |x|<={GOD_BUDGET} but high entropy > {GOD_BUDGET} cycles): {np.sum(chaos_overflow_mask)} ({np.mean(chaos_overflow_mask)*100:.2f}%) | Capped at {GOD_BUDGET} cycles")

# Compare total cycles: Unbounded Scan vs Minkowski Light-Cone Bounded Virtual CPU
unbounded_total_cycles = np.sum(required_cycles)
bounded_total_cycles = np.sum(required_cycles[timelike_mask]) + 0 * np.sum(spacelike_unreachable_mask) + GOD_BUDGET * np.sum(chaos_overflow_mask)
print(f"Total Cycles (Unbounded): {unbounded_total_cycles:,.1f}")
print(f"Total Cycles (Minkowski Bounded): {bounded_total_cycles:,.1f} (Saved {(1 - bounded_total_cycles/unbounded_total_cycles)*100:.2f}% compute!)")