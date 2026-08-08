#!/usr/bin/env python3
"""
KIS-Hyperbolic binding simulation.
กฎ: "กำลังขยายของ A ผูกกับจุด infinity ของ B, กำลังขยายของ B ผูกกับจุด infinity ของ A"
= antipodal binding — A กับ B อยู่ตรงข้ามขั้ว (antipode) บนทรงกลม
→ เมื่อ A ย่อ (เข้าหา pole/∞) B ขยาย (ออกจาก pole)
→ ใช้ทรัพยากรรวมคงที่ (ไม่กินกัน) เพราะ peak A = trough B เสมอ.
"""
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle

# ---- parameters ----
D = int(1.618 * 100)  # golden steps (phi)
theta_A = 0.0          # KIS A angle
theta_B = np.pi        # antipode (opposite pole)
N = 201                # frames
t = np.linspace(0, 2*np.pi, N)

# expansion envelope: A expands when B contracts (anti-phase sine)
env = np.sin(t)                      # range [-1,1]
R_A = 0.5 + 0.45 * np.cos(t)          # A radius of the disk-region it occupies (expansion)
R_B = 0.5 - 0.45 * np.cos(t)          # B is the exact opposite — sum is constant

# ---- figure ----
fig, axes = plt.subplots(1, 3, figsize=(15, 5))

# 1) Poincare-ish disk with A & B as antipodes exchanging "expansion"
ax = axes[0]
ax.set_aspect("equal")
ax.add_patch(Circle((0, 0), 1.0, fill=False, lw=1.5, color="k"))
# boundary = "point at infinity"
for ang in np.linspace(0, 2*np.pi, 36, endpoint=False):
    ax.scatter(np.cos(ang), np.sin(ang), s=2, color="gray", alpha=0.4)
ax.add_patch(Circle((0,0), 1.05, fill=False, lw=0.5, color="gray", ls=":"))

# traces
Ax = 0.7*np.cos(theta_A)*R_A
Ay = 0.7*np.sin(theta_A)*R_A
Bx = 0.7*np.cos(theta_B)*R_B
By = 0.7*np.sin(theta_B)*R_B
ax.plot(Ax, Ay, color="tab:red", lw=2)
ax.plot(Bx, By, color="tab:blue", lw=2)
ax.scatter([Ax[0]],[Ay[0]], s=60, color="red", label="KIS A (expand)")
ax.scatter([Bx[0]],[By[0]], s=60, color="blue", label="KIS B (contract)")
ax.set_title("Antipodal binding: A ↔ B (opposite poles)")
ax.legend(fontsize=8, loc="upper right")
ax.set_xlim(-1.3,1.3); ax.set_ylim(-1.3,1.3)

# 2) expansion magnitude over time (anti-phase → sum constant)
ax2 = axes[1]
ax2.plot(t, R_A-0.5, color="red", label="A expansion (R_A)")
ax2.plot(t, R_B-0.5, color="blue", label="B expansion (R_B)")
ax2.plot(t, (R_A+R_B)-1.0, color="green", lw=2.5, label="SUM (R_A+B_R) −1 = const")
ax2.axhline(0, color="gray", lw=0.8, ls="--")
ax2.set_title("Anti-phase: peak A = trough B, sum constant")
ax2.set_xlabel("time")
ax2.legend(fontsize=8)
ax2.grid(alpha=0.3)

# 3) resource "bytes" — if each pole's growth uses memory, anti-phase ⇒ flat total
res_A = 1.0 + 0.6*np.sin(t)          # A uses more when expanding
res_B = 1.0 - 0.6*np.sin(t)          # B uses more when contracting
ax3 = axes[2]
ax3.stackplot(t, res_A, res_B, colors=["red","blue"], alpha=0.5, labels=["A","B"])
ax3.plot(t, res_A+res_B, "k-", lw=2.5, label="total = CONSTANT")
ax3.set_title("Resource vs time: total flat (no thrashing)")
ax3.set_ylabel("resource units")
ax3.legend(fontsize=8, loc="upper right")
ax3.grid(alpha=0.3)

fig.suptitle("KIS-Hyperbolic binding: each side's ∞ = the other's anchor — total resource invariant", fontsize=13)
fig.tight_layout(rect=[0,0,1,0.95])
fig.savefig("kis_hyperbolic_binding.png", dpi=110)
print("saved kis_hyperbolic_binding.png")

# also compute evidence
print("R_A+R_B const check: max-min = %.4f"%(np.max(R_A+R_B)-np.min(R_A+R_B)))
print("resource total const: max-min = %.4f"%(np.max(res_A+res_B)-np.min(res_A+res_B)))