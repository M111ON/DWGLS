"""
Visualization — 3D structure + data flow
"""
import numpy as np
import plotly.graph_objects as go
from geometry_core import (
    ruler_tick, voxel_size, compound_144,
    geo_jump_hilbert, geo_jump_peano, geo_jump_metatron,
    geo_fibo_clock, GRID_SIZE, PHI
)

def visualize_structure():
    """3D visualization of the spherical container with data flow"""
    
    fig = go.Figure()
    
    # === Layer 0: 6ico compound (144 vertices) ===
    compound = compound_144(scale=2.0)
    fig.add_trace(go.Scatter3d(
        x=compound[:,0], y=compound[:,1], z=compound[:,2],
        mode='markers',
        name='6ico (144 verts)',
        marker=dict(size=4, color='#00c8ff', opacity=0.8,
                   line=dict(width=0.5, color='black'))
    ))
    
    # === Golden-ratio spheres (5 layers) ===
    colors = ['#ff6b35', '#ff8c42', '#ffa62b', '#ffc300', '#ffd700']
    
    for n in range(5):
        r = ruler_tick(+1, n)
        vsize = voxel_size(n)
        
        # Generate points on sphere
        n_theta = (n + 1) * 8
        n_phi = (n + 1) * 8
        
        theta = np.linspace(0, np.pi, n_theta)
        phi = np.linspace(0, 2*np.pi, n_phi)
        theta, phi = np.meshgrid(theta, phi)
        
        x = r * np.sin(theta) * np.cos(phi)
        y = r * np.sin(theta) * np.sin(phi)
        z = r * np.cos(theta)
        
        fig.add_trace(go.Scatter3d(
            x=x.flatten(), y=y.flatten(), z=z.flatten(),
            mode='markers',
            name=f'Layer {n} (r={r:.2f})',
            marker=dict(size=vsize * 10, color=colors[n], opacity=0.4,
                       line=dict(width=0.2, color='gray'))
        ))
    
    # === Data flow arrows (conceptual) ===
    # Show how data flows from center to surface
    arrow_start = [0, 0, 0]
    arrow_end = [ruler_tick(+1, 4), 0, 0]
    
    fig.add_trace(go.Scatter3d(
        x=[arrow_start[0], arrow_end[0]],
        y=[arrow_start[1], arrow_end[1]],
        z=[arrow_start[2], arrow_end[2]],
        mode='lines',
        name='Data flow (center → surface)',
        line=dict(color='red', width=4)
    ))
    
    # Add text annotations
    fig.add_annotation(
        text="Data flows FROM center (small, dense) TO surface (large, sparse)",
        xref="paper", yref="paper",
        x=0.5, y=1.1,
        showarrow=False,
        font=dict(size=14, color='black')
    )
    
    fig.update_layout(
        title='Spherical Container — Structure IS the Codec<br>'
              '<sub>6ico compound (144 verts) + golden-ratio layers</sub>',
        scene=dict(
            xaxis_title='X', yaxis_title='Y', zaxis_title='Z',
            aspectmode='data'
        ),
        legend=dict(x=0, y=1),
        width=900,
        height=700
    )
    
    fig.write_html('spherical_container_unified.html')
    print("[OK] Saved: spherical_container_unified.html")


def visualize_data_flow():
    """Visualize how data flows through the structure"""
    
    fig = go.Figure()
    
    # Show 5 layers with different colors
    colors = ['#ff6b35', '#ff8c42', '#ffa62b', '#ffc300', '#ffd700']
    names = ['Center (dense)', 'Layer 1', 'Layer 2', 'Layer 3', 'Surface (sparse)']
    
    for n in range(5):
        r = ruler_tick(+1, n)
        vsize = voxel_size(n)
        
        # Generate points on sphere
        n_points = (n + 1) * 12
        theta = np.linspace(0, np.pi, int(np.sqrt(n_points)))
        phi = np.linspace(0, 2*np.pi, int(np.sqrt(n_points)))
        theta, phi = np.meshgrid(theta, phi)
        
        x = r * np.sin(theta) * np.cos(phi)
        y = r * np.sin(theta) * np.sin(phi)
        z = r * np.cos(theta)
        
        fig.add_trace(go.Scatter3d(
            x=x.flatten(), y=y.flatten(), z=z.flatten(),
            mode='markers',
            name=names[n],
            marker=dict(size=vsize * 8, color=colors[n], opacity=0.6,
                       line=dict(width=0.3, color='black'))
        ))
    
    # Add radial lines to show expansion
    for angle in range(0, 360, 45):
        theta = np.radians(angle)
        x_line = [0, ruler_tick(+1, 4) * np.cos(theta)]
        y_line = [0, ruler_tick(+1, 4) * np.sin(theta)]
        z_line = [0, 0]
        
        fig.add_trace(go.Scatter3d(
            x=x_line, y=y_line, z=z_line,
            mode='lines',
            showlegend=False,
            line=dict(color='gray', width=1, dash='dot')
        ))
    
    fig.update_layout(
        title='Data Flow: Center (small, dense) → Surface (large, sparse)<br>'
              '<sub>Geometry IS the compression mechanism</sub>',
        scene=dict(
            xaxis_title='X', yaxis_title='Y', zaxis_title='Z',
            aspectmode='data'
        ),
        legend=dict(x=0, y=1),
        width=900,
        height=700
    )
    
    fig.write_html('data_flow_visualization.html')
    print("[OK] Saved: data_flow_visualization.html")


if __name__ == "__main__":
    visualize_structure()
    visualize_data_flow()
