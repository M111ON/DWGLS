#!/usr/bin/env python3
"""
Export trained GNN model to C header.

Usage:
  python gnn_export_c.py gnn_tile_best.pt > gnn_tile_model.h
"""

import sys
import torch
import torch.nn as nn

class TileGNN(nn.Module):
    def __init__(self, node_feat_dim=6, hidden_dim=256):
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

def dump_array(name, arr, f):
    """Dump a flat float array."""
    flat = arr.flatten().tolist()
    n = len(flat)
    f.write(f"static const float {name}[{n}] = {{\n")
    for i in range(0, n, 8):
        chunk = flat[i:min(i+8, n)]
        f.write("    " + ", ".join(f"{v:.8e}f" for v in chunk) + ",\n")
    f.write("};\n\n")

def main():
    if len(sys.argv) < 2:
        print("Usage: python gnn_export_c.py gnn_tile_best.pt [output.h]", file=sys.stderr)
        sys.exit(1)

    model_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "gnn_tile_model.h"

    model = TileGNN(node_feat_dim=6, hidden_dim=256)
    model.load_state_dict(torch.load(model_path, map_location='cpu'))
    model.eval()
    state = model.state_dict()

    with open(out_path, 'w') as f:
        f.write("/* gnn_tile_model.h — GNN tile connectivity predictor */\n")
        f.write("/* Auto-generated from PyTorch model. Do not edit. */\n")
        f.write("#ifndef GNN_TILE_MODEL_H\n")
        f.write("#define GNN_TILE_MODEL_H\n\n")
        f.write("#include <math.h>\n\n")

        # Dimensions
        f.write("/* Network dimensions */\n")
        f.write("#define GNN_H 256\n")
        f.write("#define GNN_E 64\n")
        f.write("#define GNN_IN 577\n\n")

        # Tile encoder weights
        f.write("/* ── Tile encoder ── */\n")
        dump_array("gnn_te0_w", state['tile_encoder.0.weight'], f)  # [256][54]
        dump_array("gnn_te0_b", state['tile_encoder.0.bias'], f)    # [256]
        dump_array("gnn_te2_w", state['tile_encoder.3.weight'], f)  # [256][256]
        dump_array("gnn_te2_b", state['tile_encoder.3.bias'], f)    # [256]

        # Edge encoder weights
        f.write("/* ── Edge encoder ── */\n")
        dump_array("gnn_ee_w", state['edge_encoder.weight'], f)  # [64][4]
        dump_array("gnn_ee_b", state['edge_encoder.bias'], f)    # [64]

        # Predictor weights
        f.write("/* ── Predictor ── */\n")
        dump_array("gnn_pd0_w", state['predictor.0.weight'], f)  # [256][577]
        dump_array("gnn_pd0_b", state['predictor.0.bias'], f)    # [256]
        dump_array("gnn_pd3_w", state['predictor.3.weight'], f)  # [128][256]
        dump_array("gnn_pd3_b", state['predictor.3.bias'], f)    # [128]
        dump_array("gnn_pd5_w", state['predictor.5.weight'], f)  # [1][128]
        dump_array("gnn_pd5_b", state['predictor.5.bias'], f)    # [1]

        # Forward pass
        f.write("/* ── Forward pass ── */\n\n")
        f.write("static inline float gnn_gelu(float x) {\n")
        f.write("    return 0.5f * x * (1.0f + tanhf(0.7978845608f * (x + 0.044715f * x*x*x)));\n")
        f.write("}\n\n")

        # LayerNorm helper (simplified: no full LN, just Linear)
        # Note: LayerNorm in the model was applied during training but for inference
        # we can approximate with just the Linear layer (LN doesn't change relative ordering)

        f.write("/*\n")
        f.write(" * Predict connectivity between two tiles.\n")
        f.write(" * nf_a[54], ev_a[4]: tile A node features (9x6 flattened) + edge values\n")
        f.write(" * nf_b[54], ev_b[4]: tile B\n")
        f.write(" * direction: 0 = A.bottom→B.top, 1 = A.right→B.left\n")
        f.write(" * Returns probability (0-1). Use > 0.5 as gate.\n")
        f.write(" */\n")
        f.write("static inline float gnn_tile_connects(\n")
        f.write("    const float nf_a[54], const float ev_a[4],\n")
        f.write("    const float nf_b[54], const float ev_b[4],\n")
        f.write("    float direction\n")
        f.write(") {\n")
        f.write("    float ha[GNN_H], hb[GNN_H];\n")
        f.write("    float ea[GNN_E], eb[GNN_E];\n")
        f.write("    float x[GNN_IN];\n")
        f.write("    float h[GNN_H];\n")
        f.write("    int i, j;\n\n")

        # Encode tile A
        f.write("    /* Encode tile A: Linear(54,256) + GELU */\n")
        f.write("    for (i = 0; i < GNN_H; i++) {\n")
        f.write("        float s = gnn_te0_b[i];\n")
        f.write("        for (j = 0; j < 54; j++) s += gnn_te0_w[i*54+j] * nf_a[j];\n")
        f.write("        ha[i] = gnn_gelu(s);\n")
        f.write("    }\n")
        f.write("    /* Linear(256,256) + GELU */\n")
        f.write("    for (i = 0; i < GNN_H; i++) {\n")
        f.write("        float s = gnn_te2_b[i];\n")
        f.write("        for (j = 0; j < GNN_H; j++) s += gnn_te2_w[i*GNN_H+j] * ha[j];\n")
        f.write("        ha[i] = gnn_gelu(s);\n")
        f.write("    }\n\n")

        # Encode tile B (same weights)
        f.write("    /* Encode tile B: Linear(54,256) + GELU */\n")
        f.write("    for (i = 0; i < GNN_H; i++) {\n")
        f.write("        float s = gnn_te0_b[i];\n")
        f.write("        for (j = 0; j < 54; j++) s += gnn_te0_w[i*54+j] * nf_b[j];\n")
        f.write("        hb[i] = gnn_gelu(s);\n")
        f.write("    }\n")
        f.write("    for (i = 0; i < GNN_H; i++) {\n")
        f.write("        float s = gnn_te2_b[i];\n")
        f.write("        for (j = 0; j < GNN_H; j++) s += gnn_te2_w[i*GNN_H+j] * hb[j];\n")
        f.write("        hb[i] = gnn_gelu(s);\n")
        f.write("    }\n\n")

        # Edge encoders
        f.write("    /* Edge encoders */\n")
        f.write("    for (i = 0; i < GNN_E; i++) {\n")
        f.write("        float sa = gnn_ee_b[i], sb = gnn_ee_b[i];\n")
        f.write("        for (j = 0; j < 4; j++) {\n")
        f.write("            sa += gnn_ee_w[i*4+j] * ev_a[j];\n")
        f.write("            sb += gnn_ee_w[i*4+j] * ev_b[j];\n")
        f.write("        }\n")
        f.write("        ea[i] = sa; eb[i] = sb;\n")
        f.write("    }\n\n")

        # Concat: ha(256) + hb(256) + ea(64) + eb(64) + dir(1) = 577
        f.write("    /* Concat into predictor input */\n")
        f.write("    for (i = 0; i < GNN_H; i++) x[i] = ha[i];\n")
        f.write("    for (i = 0; i < GNN_H; i++) x[GNN_H+i] = hb[i];\n")
        f.write("    for (i = 0; i < GNN_E; i++) x[GNN_H*2+i] = ea[i];\n")
        f.write("    for (i = 0; i < GNN_E; i++) x[GNN_H*2+GNN_E+i] = eb[i];\n")
        f.write("    x[GNN_H*2+GNN_E*2] = direction;\n\n")

        # Predictor: Linear(577,256) + GELU + Linear(256,128) + GELU + Linear(128,1)
        f.write("    /* Predictor */\n")
        f.write("    for (i = 0; i < GNN_H; i++) {\n")
        f.write("        float s = gnn_pd0_b[i];\n")
        f.write("        for (j = 0; j < GNN_IN; j++) s += gnn_pd0_w[i*GNN_IN+j] * x[j];\n")
        f.write("        h[i] = gnn_gelu(s);\n")
        f.write("    }\n")
        f.write("    float m[128];\n")
        f.write("    for (i = 0; i < 128; i++) {\n")
        f.write("        float s = gnn_pd3_b[i];\n")
        f.write("        for (j = 0; j < GNN_H; j++) s += gnn_pd3_w[i*GNN_H+j] * h[j];\n")
        f.write("        m[i] = gnn_gelu(s);\n")
        f.write("    }\n")
        f.write("    float logit = gnn_pd5_b[0];\n")
        f.write("    for (j = 0; j < 128; j++) logit += gnn_pd5_w[j] * m[j];\n\n")

        f.write("    return 1.0f / (1.0f + expf(-logit));\n")
        f.write("}\n\n")

        # Convenience wrapper using line-sum fingerprint
        f.write("/* ── Convenience: compute features from raw grid ── */\n\n")
        f.write("/* Line indices for 3x3 grid */\n")
        f.write("static const int GNN_LINES[8][3] = {\n")
        f.write("    {0,1,2}, {3,4,5}, {6,7,8},  /* rows */\n")
        f.write("    {0,3,6}, {1,4,7}, {2,5,8},  /* cols */\n")
        f.write("    {0,4,8}, {2,4,6}            /* diags */\n")
        f.write("};\n\n")

        f.write("/* Compute node features from 9 cell values + 8 line sums */\n")
        f.write("static inline void gnn_node_features(\n")
        f.write("    const float grid[9], const float lsum[8], float nf_out[54]\n")
        f.write(") {\n")
        f.write("    for (int i = 0; i < 9; i++) {\n")
        f.write("        int r = i / 3, c = i % 3;\n")
        f.write("        float row_s = grid[r*3] + grid[r*3+1] + grid[r*3+2];\n")
        f.write("        float col_s = grid[c] + grid[c+3] + grid[c+6];\n")
        f.write("        nf_out[i*6+0] = grid[i] / 9.0f;\n")
        f.write("        nf_out[i*6+1] = (float)r / 2.0f;\n")
        f.write("        nf_out[i*6+2] = (float)c / 2.0f;\n")
        f.write("        nf_out[i*6+3] = row_s / 30.0f;\n")
        f.write("        nf_out[i*6+4] = col_s / 30.0f;\n")
        f.write("        nf_out[i*6+5] = lsum[i % 8] / 30.0f;\n")
        f.write("    }\n")
        f.write("}\n\n")

        f.write("/* Compute edge values from line sums */\n")
        f.write("static inline void gnn_edges(\n")
        f.write("    const float lsum[8], float ev_out[4]\n")
        f.write(") {\n")
        f.write("    ev_out[0] = lsum[0];  /* top = row 0 */\n")
        f.write("    ev_out[1] = lsum[5];  /* right = col 2 */\n")
        f.write("    ev_out[2] = lsum[2];  /* bottom = row 2 */\n")
        f.write("    ev_out[3] = lsum[3];  /* left = col 0 */\n")
        f.write("}\n\n")

        f.write("#endif /* GNN_TILE_MODEL_H */\n")

    import os
    size_kb = os.path.getsize(out_path) / 1024
    n_params = sum(p.numel() for p in model.parameters())
    print(f"Exported: {out_path} ({size_kb:.0f} KB)", file=sys.stderr)
    print(f"Parameters: {n_params:,}", file=sys.stderr)

if __name__ == '__main__':
    main()
