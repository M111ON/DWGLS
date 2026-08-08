#!/usr/bin/env python3
"""
Residual Space + GPS Freeze — Simulation
กฎ: "ข้อมูลอยู่ที่เดิม แต่ประตูหน้าบ้านหายไป"

- Node วิ่งตามเฟรม (ค่าเปลี่ยนตลอดเวลา)
- เมื่อถึง boundary (17³) → ผลักออก → residual_space
- ค่า freeze (ไม่เปลี่ยน) — ข้อมูลอยู่ที่เดิม แต่เข้าถึงไม่ได้
- GPS = global broadcast bond → หาเจอได้เสมอ → unfreeze ได้

เปรียบเทียบ: freeze-by-structure (ไม่ล็อก, ไม่เสีย data) vs lock
"""
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# --- Simulation parameters ---
N_NODES = 12           # จำนวน nodes
FRAME_COUNT = 50       # จำนวนเฟรม
BOUNDARY = 17          # cubic boundary (17³ = 4913)

# --- Initialize nodes ---
np.random.seed(42)
node_names = [f"N{i}" for i in range(N_NODES)]
node_colors = plt.cm.Set2(np.linspace(0, 1, N_NODES))

# Node data: each node's value evolves per frame
# Active: value changes each frame (simulating "system running")
# Frozen: value stays constant after freeze
np.random.seed(7)
node_values = np.zeros((FRAME_COUNT, N_NODES))
node_values[0] = np.random.uniform(-1, 1, N_NODES) * 10

# Node positions in cubic space (x, y, z)
node_pos = np.random.uniform(0, BOUNDARY, (FRAME_COUNT, N_NODES, 3))

# Track freeze events
frozen_at = [None] * N_NODES   # frame when frozen
gps_set = [None] * N_NODES     # GPS address when frozen
frozen_data = [None] * N_NODES # frozen value

# Run simulation
for f in range(1, FRAME_COUNT):
    for n in range(N_NODES):
        if frozen_at[n] is not None:
            # FROZEN: value stays constant, position stays
            node_values[f, n] = frozen_data[n]
            node_pos[f, n] = node_pos[f-1, n]
        else:
            # ACTIVE: value evolves (random walk), position evolves
            node_values[f, n] = node_values[f-1, n] + np.random.normal(0, 0.5)
            # Position moves toward boundary
            node_pos[f, n] = node_pos[f-1, n] + np.random.normal(0, 0.8)
            
            # Check boundary: if any coordinate >= BOUNDARY → FREEZE
            max_coord = np.max(np.abs(node_pos[f, n]))
            if max_coord >= BOUNDARY:
                frozen_at[n] = f
                frozen_data[n] = node_values[f, n]
                gps_set[n] = f"GPS-{node_names[n]}@{f}"
                node_pos[f, n] = node_pos[f-1, n]  # stay at last valid position

# --- FIGURE ---
fig = plt.figure(figsize=(18, 10))

# Panel 1: 3D cubic space — nodes, boundary, frozen
ax1 = fig.add_subplot(221, projection='3d')
# Draw cubic boundary
r = [0, BOUNDARY]
for s, e in [(0,1),(0,2),(0,4),(1,3),(2,3),(4,5),(5,7),(6,7)]:
    pass
# simplified boundary wireframe
for coord in [0, BOUNDARY]:
    for i in range(2):
        for j in range(2):
            xs = [coord, coord]
            ys = [0 if i==0 else BOUNDARY, 0 if i==0 else BOUNDARY]
            zs = [0 if j==0 else BOUNDARY, 0 if j==0 else BOUNDARY]
            ax1.plot(xs, [0 if i==0 else BOUNDARY]*2, zs, 'k-', alpha=0.15)
            ax1.plot([coord]*2, [0 if i==0 else BOUNDARY]*2, zs, 'k-', alpha=0.15)
            ax1.plot(xs, [0 if i==0 else BOUNDARY]*2, [coord]*2, 'k-', alpha=0.15)

# Plot active nodes (blue) and frozen nodes (red)
frozen_idx = [n for n in range(N_NODES) if frozen_at[n] is not None]
active_idx = [n for n in range(N_NODES) if frozen_at[n] is None]
final_frame = FRAME_COUNT - 1

for n in active_idx:
    ax1.scatter(*node_pos[final_frame, n], s=80, c=[node_colors[n]], 
                edgecolors='navy', linewidths=1.5, label=f"{node_names[n]} active")
for n in frozen_idx:
    ax1.scatter(*node_pos[frozen_at[n], n], s=120, c='red', marker='x', linewidths=3,
                label=f"{node_names[n]} FROZEN@{frozen_at[n]}")

ax1.set_xlabel('X')
ax1.set_ylabel('Y')
ax1.set_zlabel('Z')
ax1.set_title(f'Cubic Space (boundary={BOUNDARY})\n{len(active_idx)} active, {len(frozen_idx)} frozen')
ax1.legend(fontsize=7, loc='upper left')

# Panel 2: Node values over time — active vs frozen
ax2 = fig.add_subplot(222)
for n in range(N_NODES):
    if frozen_at[n] is not None:
        f = frozen_at[n]
        ax2.plot(range(f+1), node_values[:f+1, n], color=node_colors[n], lw=1.5)
        ax2.plot(range(f, FRAME_COUNT), [frozen_data[n]]*(FRAME_COUNT-f), 
                color='red', lw=2, ls='--', alpha=0.7)
        ax2.axvline(f, color='red', ls=':', alpha=0.4)
        ax2.text(f+1, frozen_data[n]+0.3, f'FREEZE\nGPS:{gps_set[n][:8]}...', 
                fontsize=6, color='red', rotation=0)
    else:
        ax2.plot(range(FRAME_COUNT), node_values[:, n], color=node_colors[n], lw=1, alpha=0.7)

ax2.set_xlabel('Frame')
ax2.set_ylabel('Value')
ax2.set_title('Node values over time\n(solid=active, dashed=frozen=constant)')
ax2.grid(alpha=0.3)

# Panel 3: GPS Freeze mechanism — 3 states
ax3 = fig.add_subplot(223)
states = ['Active\n(in system)', 'Frozen\n(door gone, GPS set)', 'Unfreeze\n(door back, GPS found)']
colors = ['#4CAF50', '#f44336', '#2196F3']
y_pos = [0.5, 0.5, 0.5]
for i, (st, col) in enumerate(zip(states, colors)):
    rect = mpatches.FancyBboxPatch((i*3.5, 0.2), 3, 0.6, boxstyle="round,pad=0.1",
                                    facecolor=col, alpha=0.3, edgecolor=col, lw=2)
    ax3.add_patch(rect)
    ax3.text(i*3.5+1.5, 0.5, st, ha='center', va='center', fontsize=10, fontweight='bold')
    if i < 2:
        ax3.annotate('', xy=((i+1)*3.5-0.2, 0.5), xytext=(i*3.5+3+0.2, 0.5),
                    arrowprops=dict(arrowstyle='->', color='gray', lw=2))
# arrows with labels
ax3.text(1.75, 0.05, 'boundary hit', ha='center', color='gray', fontsize=8)
ax3.text(5.25, 0.05, 'GPS lookup', ha='center', color='gray', fontsize=8)
ax3.set_xlim(-0.5, 11)
ax3.set_ylim(-0.1, 1.0)
ax3.set_title('Freeze-by-Structure lifecycle')
ax3.axis('off')

# Panel 4: Comparison table
ax4 = fig.add_subplot(224)
ax4.axis('off')
table_data = [
    ['Freeze-by-Structure', 'Freeze-by-Lock'],
    ['Data stays at original place', 'Must copy/move data'],
    ['Door gone (access cut)', 'Lock (block all access)'],
    ['Zero data loss', 'Deadlock risk'],
    ['Unfreeze = open door back', 'Unfreeze = release lock'],
    ['GPS finds it always', 'Lock holder must release'],
    ['System never stops', 'System waits for unlock'],
]
table = ax4.table(cellText=table_data, loc='center', cellLoc='center')
table.auto_set_font_size(False)
table.set_fontsize(9)
table.scale(1, 1.8)
for (row, col), cell in table.get_celld().items():
    if row == 0:
        cell.set_facecolor('#E8EAF6')
        cell.set_text_props(fontweight='bold')
    elif col == 0:
        cell.set_facecolor('#E8F5E9')  # green tint
    elif col == 1:
        cell.set_facecolor('#FFEBEE')  # red tint
ax4.set_title('Freeze-by-Structure vs Freeze-by-Lock')

fig.suptitle('Residual Space + GPS Freeze: "Data stays, door disappears"', fontsize=13, fontweight='bold')
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig("residual_gps_freeze.png", dpi=110)
print("saved residual_gps_freeze.png")

# Print summary
print(f"\nTotal nodes: {N_NODES}")
print(f"Active (frame {final_frame}): {len(active_idx)}")
print(f"Frozen: {len(frozen_idx)}")
for n in frozen_idx:
    print(f"  {node_names[n]}: frozen@frame {frozen_at[n]}, value={frozen_data[n]:.2f}, GPS={gps_set[n]}")
print(f"\nKey proof: frozen node values stay CONSTANT after freeze = 'door gone but house stays'")