import numpy as np
import plotly.graph_objects as go

# ===== โครงสร้างเดิมของคุณ =====
alpha = 0.381966
R0 = 1.0
N = 4  # จำนวน generation ต่อแกน

def ruler_tick(sign, n):
    return sign * R0 * (1 + alpha) ** n

def voxel_size(n):
    return abs(ruler_tick(+1, n+1) - ruler_tick(+1, n))

def address_to_xyz(sx,nx, sy,ny, sz,nz):
    return (ruler_tick(sx,nx), ruler_tick(sy,ny), ruler_tick(sz,nz))

# ===== 8 octants =====
octants = [(sx,sy,sz)
           for sx in (+1,-1) for sy in (+1,-1) for sz in (+1,-1)]

icosa  = dict(x=[], y=[], z=[], s=[])
dodeca = dict(x=[], y=[], z=[], s=[])

for (sx,sy,sz) in octants:
    for nx in range(N):
        for ny in range(N):
            for nz in range(N):
                x,y,z = address_to_xyz(sx,nx, sy,ny, sz,nz)
                size  = voxel_size(max(nx,ny,nz))**3 * 40
                if (nx+ny+nz) % 2 == 0:
                    icosa['x'].append(x); icosa['y'].append(y)
                    icosa['z'].append(z); icosa['s'].append(size)
                else:
                    dodeca['x'].append(x); dodeca['y'].append(y)
                    dodeca['z'].append(z); dodeca['s'].append(size)

fig = go.Figure()

fig.add_trace(go.Scatter3d(
    x=icosa['x'], y=icosa['y'], z=icosa['z'],
    mode='markers', name='icosa(20) — parity even',
    marker=dict(size=icosa['s'], color='#00c8ff', opacity=0.85,
                line=dict(width=0.5, color='black'))))

fig.add_trace(go.Scatter3d(
    x=dodeca['x'], y=dodeca['y'], z=dodeca['z'],
    mode='markers', name='dodeca(12) — parity odd',
    marker=dict(size=dodeca['s'], color='#ff6b35', opacity=0.85,
                line=dict(width=0.5, color='black'))))

# ป้ายชื่อ 8 octants ที่มุม outermost
for i,(sx,sy,sz) in enumerate(octants):
    corner = address_to_xyz(sx,N-1, sy,N-1, sz,N-1)
    fig.add_trace(go.Scatter3d(
        x=[corner[0]], y=[corner[1]], z=[corner[2]],
        mode='text', showlegend=False,
        text=[f'octant {i+1}'], textposition='top center',
        marker=dict(size=1), textfont=dict(size=10, color='gray')))

fig.update_layout(
    title='Golden-Ratio Voxel World — 8 octants, หมุนได้',
    scene=dict(
        xaxis_title='X', yaxis_title='Y', zaxis_title='Z',
        aspectmode='data'),
    legend=dict(x=0, y=1))

fig.show()   # เปิด browser ให้หมุน/ซูม/ลาก ได้เลย
print("เปิดหน้าต่าง browser แล้วหมุนดูได้เลยครับ")