#!/usr/bin/env python3
"""
KIS-Hyperbolic binding — 3 AXES (X, Y, Z).
User built "XYZ" from 3 KIS axes. Generalizes the antipodal binding to 3-phase.
KEY: 3 mutually orthogonal KIS axes, each oscillating expansion/contraction
     120° apart → instantaneous TOTAL resource = EXACTLY constant (three-phase power).
Greatly smoother than single antipodal pair (which has 2x ripple).
"""
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

D = 1.618
N = 601
t = np.linspace(0, 4*np.pi, N)

# three-phase expansion: 120° apart
phase = np.array([0, 2*np.pi/3, 4*np.pi/3])
R = np.array([0.5 + 0.45*np.cos(t+ph) for ph in phase])   # 3xN  (axis expansion)
sumR = R.sum(axis=0)

# three-phase resource (like 3-phase AC power)
res = np.array([1.0 + 0.6*np.sin(t+ph) for ph in phase])
sumRes = res.sum(axis=0)

# ---- figure ----
fig = plt.figure(figsize=(16, 5.5))

# 1) 3 mutually-orthogonal axes on sphere (antipodal along each axis)
ax = fig.add_subplot(1, 3, 1, projection="3d")
u = np.linspace(0, np.pi, 20); v = np.linspace(0, 2*np.pi, 30)
xs = np.outer(np.sin(u), np.cos(v)); ys = np.outer(np.sin(u), np.sin(v)); zs = np.outer(np.cos(u), np.ones_like(v))
ax.plot_surface(xs, ys, zs, color="lightgray", alpha=0.25, rstride=1, cstride=1, edgecolor="none")
# 3 axes = 3 KIS, each with a positive(+) and negative(-) pole = 6 half-axes (DiamondBlock faces!)
for i, (nm, col) in enumerate([("X","red"),("Y","green"),("Z","blue")]):
    d = np.array([1.0,0,0]) if nm=="X" else (np.array([0,1,0]) if nm=="Y" else np.array([0,0,1]))
    ax.plot([-d[0],d[0]],[-d[1],d[1]],[-d[2],d[2]], color=col, lw=2.5)
    ax.text(d[0]*1.2, d[1]*1.2, d[2]*1.2, nm, color=col, fontsize=14, fontweight="bold")
ax.set_title("3 KIS axes → XYZ (6 half-axes)")
ax.set_xlim(-1.3,1.3); ax.set_ylim(-1.3,1.3); ax.set_zlim(-1.3,1.3)

# 2) three-phase expansion — sum EXACTLY constant (=1.5)
ax2 = fig.add_subplot(1, 3, 2)
for i, nm in enumerate(["X","Y","Z"]):
    ax2.plot(t, R[i]-0.5, lw=1.6, label=f"{nm} expansion")
ax2.plot(t, sumR-1.5, "k-", lw=2.8, label="SUM −1.5")
ax2.axhline(0, color="gray", ls="--", lw=0.8)
ax2.set_title("Three-phase: sum EXACTLY constant")
ax2.set_xlabel("time"); ax2.legend(fontsize=8); ax2.grid(alpha=0.3)

# 3) resource stack — three-phase power, total perfectly flat
ax3 = fig.add_subplot(1, 3, 3)
ax3.stackplot(t, res[0], res[1], res[2], colors=["red","green","blue"], alpha=0.5, labels=["X","Y","Z"])
ax3.plot(t, sumRes, "k-", lw=2.8, label="total = const")
ax3.set_title("3-phase resource: total flat (no ripple)")
ax3.set_ylabel("resource units"); ax3.legend(fontsize=9, loc="upper right"); ax3.grid(alpha=0.3)

fig.suptitle("KIS 3-AXIS (X·Y·Z): three-phase antipodal binding — total resource INVARIANT", fontsize=13)
fig.tight_layout(rect=[0,0,1,0.95])
fig.savefig("kis_3axis_hyperbolic.png", dpi=110)
print("saved kis_3axis_hyperbolic.png")
print("sum expansion const: max-min = %.2e"% (np.max(sumR)-np.min(sumR)))
print("sum resource  const: max-min = %.2e"%(np.max(sumRes)-np.min(sumRes)))
# compare ripple with 1-axis (single antipode had 2x ripple)
single = np.abs(np.cos(t)).max()
print("single-axis amplitude peak = %.3f  | 3-axis residual = %.2e"%(single, np.max(sumRes-3.0)))