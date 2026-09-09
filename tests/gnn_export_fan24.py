#!/usr/bin/env python3
"""
Export trained GNN+Fan24 model to C header.

Usage:
  python gnn_export_fan24.py gnn_fan24_best.pt > gnn_fan24_model.h
"""

import sys, math
import torch
import torch.nn as nn

FG_RING = 24
FG_WHEEL_KIS = 8
FG_WHEEL_HYP = 3
FG_FULL = 20736
N_LANGUAGES = 9
NODE_FEAT_DIM = 12

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

def dump_array(name, arr, f):
    flat = arr.flatten().tolist()
    n = len(flat)
    f.write(f"static const float {name}[{n}] = {{\n")
    for i in range(0, n, 8):
        chunk = flat[i:min(i+8, n)]
        f.write("    " + ", ".join(f"{v:.8e}f" for v in chunk) + ",\n")
    f.write("};\n\n")

def main():
    if len(sys.argv) < 2:
        print("Usage: python gnn_export_fan24.py gnn_fan24_best.pt [output.h]", file=sys.stderr)
        sys.exit(1)

    model_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "gnn_fan24_model.h"

    model = TileGNN(node_feat_dim=NODE_FEAT_DIM, hidden_dim=256)
    model.load_state_dict(torch.load(model_path, map_location='cpu'))
    model.eval()
    state = model.state_dict()

    with open(out_path, 'w') as f:
        f.write("/* gnn_fan24_model.h — GNN tile connectivity predictor with Fan24 context */\n")
        f.write("/* Auto-generated from PyTorch model. Do not edit. */\n")
        f.write("#ifndef GNN_FAN24_MODEL_H\n")
        f.write("#define GNN_FAN24_MODEL_H\n\n")
        f.write("#include <math.h>\n#include <stdint.h>\n\n")

        f.write("/* Network dimensions */\n")
        f.write("#define GNN_F24_H 256\n")
        f.write("#define GNN_F24_E 64\n")
        f.write("#define GNN_F24_NF 12\n")
        f.write("#define GNN_F24_IN 641\n\n")

        f.write("/* Fan24 constants */\n")
        f.write("#define F24_RING 24\n")
        f.write("#define F24_WHEEL_KIS 8\n")
        f.write("#define F24_WHEEL_HYP 3\n")
        f.write("#define F24_FULL 20736\n")
        f.write("#define F24_NLANG 9\n\n")

        # Tile encoder: Linear(108,256) → LayerNorm(256) → GELU → Linear(256,256) → LayerNorm(256) → GELU
        f.write("/* ── Tile encoder ── */\n")
        dump_array("gnn_f24_te0_w", state['tile_encoder.0.weight'], f)   # Linear(108,256)
        dump_array("gnn_f24_te0_b", state['tile_encoder.0.bias'], f)
        dump_array("gnn_f24_te1_w", state['tile_encoder.1.weight'], f)   # LayerNorm(256)
        dump_array("gnn_f24_te1_b", state['tile_encoder.1.bias'], f)
        dump_array("gnn_f24_te3_w", state['tile_encoder.3.weight'], f)   # Linear(256,256)
        dump_array("gnn_f24_te3_b", state['tile_encoder.3.bias'], f)
        dump_array("gnn_f24_te4_w", state['tile_encoder.4.weight'], f)   # LayerNorm(256)
        dump_array("gnn_f24_te4_b", state['tile_encoder.4.bias'], f)

        # Edge encoder
        f.write("/* ── Edge encoder ── */\n")
        dump_array("gnn_f24_ee_w", state['edge_encoder.weight'], f)
        dump_array("gnn_f24_ee_b", state['edge_encoder.bias'], f)

        # Predictor: Linear(577,256) → LayerNorm(256) → GELU → Dropout → Linear(256,128) → GELU → Linear(128,1)
        f.write("/* ── Predictor ── */\n")
        dump_array("gnn_f24_pd0_w", state['predictor.0.weight'], f)   # Linear(577,256)
        dump_array("gnn_f24_pd0_b", state['predictor.0.bias'], f)
        dump_array("gnn_f24_pd1_w", state['predictor.1.weight'], f)   # LayerNorm(256)
        dump_array("gnn_f24_pd1_b", state['predictor.1.bias'], f)
        dump_array("gnn_f24_pd4_w", state['predictor.4.weight'], f)   # Linear(256,128)
        dump_array("gnn_f24_pd4_b", state['predictor.4.bias'], f)
        dump_array("gnn_f24_pd6_w", state['predictor.6.weight'], f)   # Linear(128,1)
        dump_array("gnn_f24_pd6_b", state['predictor.6.bias'], f)

        # C forward pass
        f.write("/* ── Forward pass ── */\n\n")
        f.write("static inline float gnn_f24_gelu(float x) {\n")
        f.write("    return 0.5f * x * (1.0f + tanhf(0.7978845608f * (x + 0.044715f * x*x*x)));\n")
        f.write("}\n\n")

        # LayerNorm helper
        f.write("static inline void gnn_f24_layernorm(\n")
        f.write("    const float x[256], const float w[256], const float b[256], float y[256]\n")
        f.write(") {\n")
        f.write("    float mean = 0.0f;\n")
        f.write("    int i;\n")
        f.write("    for (i = 0; i < 256; i++) mean += x[i];\n")
        f.write("    mean /= 256.0f;\n")
        f.write("    float var = 0.0f;\n")
        f.write("    for (i = 0; i < 256; i++) { float d = x[i] - mean; var += d * d; }\n")
        f.write("    var /= 256.0f;\n")
        f.write("    float inv = 1.0f / sqrtf(var + 1e-5f);\n")
        f.write("    for (i = 0; i < 256; i++) y[i] = (x[i] - mean) * inv * w[i] + b[i];\n")
        f.write("}\n\n")

        # Fan24 feature computation
        f.write("/*\n")
        f.write(" * Compute 6 Fan24 features from tile index.\n")
        f.write(" */\n")
        f.write("static inline void gnn_f24_features(\n")
        f.write("    uint32_t tile_idx, float f24_out[6]\n")
        f.write(") {\n")
        f.write("    uint32_t pos = tile_idx % F24_FULL;\n")
        f.write("    uint32_t gear = pos % F24_RING;\n")
        f.write("    uint32_t dc = gear % F24_WHEEL_KIS;\n")
        f.write("    uint32_t dx = gear % F24_WHEEL_HYP;\n")
        f.write("    uint32_t lang = pos % F24_NLANG;\n")
        f.write("    float angle = 6.283185307f * (float)gear / (float)F24_RING;\n")
        f.write("    f24_out[0] = sinf(angle);\n")
        f.write("    f24_out[1] = cosf(angle);\n")
        f.write("    f24_out[2] = (float)dc / 7.0f;\n")
        f.write("    f24_out[3] = (float)dx / 2.0f;\n")
        f.write("    f24_out[4] = (float)lang / 8.0f;\n")
        f.write("    float center = (float)(F24_FULL / 2);\n")
        f.write("    f24_out[5] = fabsf((float)pos - center) / center;\n")
        f.write("}\n\n")

        # Node features
        f.write("static inline void gnn_f24_node_features(\n")
        f.write("    const float grid[9], const float lsum[8],\n")
        f.write("    uint32_t tile_idx, float nf_out[108]\n")
        f.write(") {\n")
        f.write("    float f24[6];\n")
        f.write("    gnn_f24_features(tile_idx, f24);\n")
        f.write("    for (int i = 0; i < 9; i++) {\n")
        f.write("        int r = i / 3, c = i % 3;\n")
        f.write("        float row_s = grid[r*3] + grid[r*3+1] + grid[r*3+2];\n")
        f.write("        float col_s = grid[c] + grid[c+3] + grid[c+6];\n")
        f.write("        nf_out[i*12+0] = grid[i] / 9.0f;\n")
        f.write("        nf_out[i*12+1] = (float)r / 2.0f;\n")
        f.write("        nf_out[i*12+2] = (float)c / 2.0f;\n")
        f.write("        nf_out[i*12+3] = row_s / 30.0f;\n")
        f.write("        nf_out[i*12+4] = col_s / 30.0f;\n")
        f.write("        nf_out[i*12+5] = lsum[i % 8] / 30.0f;\n")
        f.write("        for (int k = 0; k < 6; k++)\n")
        f.write("            nf_out[i*12+6+k] = f24[k];\n")
        f.write("    }\n")
        f.write("}\n\n")

        # Edge values
        f.write("static inline void gnn_f24_edges(\n")
        f.write("    const float lsum[8], float ev_out[4]\n")
        f.write(") {\n")
        f.write("    ev_out[0] = lsum[0];\n")
        f.write("    ev_out[1] = lsum[5];\n")
        f.write("    ev_out[2] = lsum[2];\n")
        f.write("    ev_out[3] = lsum[3];\n")
        f.write("}\n\n")

        # Main prediction function with correct LayerNorm placement
        f.write("static inline float gnn_f24_tile_connects(\n")
        f.write("    const float nf_a[108], const float ev_a[4],\n")
        f.write("    const float nf_b[108], const float ev_b[4],\n")
        f.write("    float direction\n")
        f.write(") {\n")
        f.write("    float ha[GNN_F24_H], hb[GNN_F24_H];\n")
        f.write("    float ea[GNN_F24_E], eb[GNN_F24_E];\n")
        f.write("    float x[GNN_F24_IN];\n")
        f.write("    float h[GNN_F24_H];\n")
        f.write("    int i, j;\n\n")

        # Encode tile A: Linear → LayerNorm → GELU → Linear → LayerNorm → GELU
        f.write("    /* Tile A: Linear(108,256) → LN → GELU → Linear(256,256) → LN → GELU */\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) {\n")
        f.write("        float s = gnn_f24_te0_b[i];\n")
        f.write("        for (j = 0; j < 108; j++) s += gnn_f24_te0_w[i*108+j] * nf_a[j];\n")
        f.write("        ha[i] = s;\n")
        f.write("    }\n")
        f.write("    gnn_f24_layernorm(ha, gnn_f24_te1_w, gnn_f24_te1_b, h);\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) ha[i] = gnn_f24_gelu(h[i]);\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) {\n")
        f.write("        float s = gnn_f24_te3_b[i];\n")
        f.write("        for (j = 0; j < GNN_F24_H; j++) s += gnn_f24_te3_w[i*GNN_F24_H+j] * ha[j];\n")
        f.write("        ha[i] = s;\n")
        f.write("    }\n")
        f.write("    gnn_f24_layernorm(ha, gnn_f24_te4_w, gnn_f24_te4_b, h);\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) ha[i] = gnn_f24_gelu(h[i]);\n\n")

        # Encode tile B (same weights)
        f.write("    /* Tile B: same weights */\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) {\n")
        f.write("        float s = gnn_f24_te0_b[i];\n")
        f.write("        for (j = 0; j < 108; j++) s += gnn_f24_te0_w[i*108+j] * nf_b[j];\n")
        f.write("        hb[i] = s;\n")
        f.write("    }\n")
        f.write("    gnn_f24_layernorm(hb, gnn_f24_te1_w, gnn_f24_te1_b, h);\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) hb[i] = gnn_f24_gelu(h[i]);\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) {\n")
        f.write("        float s = gnn_f24_te3_b[i];\n")
        f.write("        for (j = 0; j < GNN_F24_H; j++) s += gnn_f24_te3_w[i*GNN_F24_H+j] * hb[j];\n")
        f.write("        hb[i] = s;\n")
        f.write("    }\n")
        f.write("    gnn_f24_layernorm(hb, gnn_f24_te4_w, gnn_f24_te4_b, h);\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) hb[i] = gnn_f24_gelu(h[i]);\n\n")

        # Edge encoders
        f.write("    /* Edge encoders */\n")
        f.write("    for (i = 0; i < GNN_F24_E; i++) {\n")
        f.write("        float sa = gnn_f24_ee_b[i], sb = gnn_f24_ee_b[i];\n")
        f.write("        for (j = 0; j < 4; j++) {\n")
        f.write("            sa += gnn_f24_ee_w[i*4+j] * ev_a[j];\n")
        f.write("            sb += gnn_f24_ee_w[i*4+j] * ev_b[j];\n")
        f.write("        }\n")
        f.write("        ea[i] = sa; eb[i] = sb;\n")
        f.write("    }\n\n")

        # Concat
        f.write("    /* Concat: ha(256) + hb(256) + ea(64) + eb(64) + dir(1) = 577 */\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) x[i] = ha[i];\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) x[GNN_F24_H+i] = hb[i];\n")
        f.write("    for (i = 0; i < GNN_F24_E; i++) x[GNN_F24_H*2+i] = ea[i];\n")
        f.write("    for (i = 0; i < GNN_F24_E; i++) x[GNN_F24_H*2+GNN_F24_E+i] = eb[i];\n")
        f.write("    x[GNN_F24_H*2+GNN_F24_E*2] = direction;\n\n")

        # Predictor: Linear(577,256) → LN → GELU → Linear(256,128) → GELU → Linear(128,1)
        f.write("    /* Predictor: Linear(577,256) → LN → GELU → Linear(256,128) → GELU → Linear(128,1) */\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) {\n")
        f.write("        float s = gnn_f24_pd0_b[i];\n")
        f.write("        for (j = 0; j < GNN_F24_IN; j++) s += gnn_f24_pd0_w[i*GNN_F24_IN+j] * x[j];\n")
        f.write("        h[i] = s;\n")
        f.write("    }\n")
        f.write("    gnn_f24_layernorm(h, gnn_f24_pd1_w, gnn_f24_pd1_b, x);\n")
        f.write("    for (i = 0; i < GNN_F24_H; i++) x[i] = gnn_f24_gelu(x[i]);\n\n")

        f.write("    float m[128];\n")
        f.write("    for (i = 0; i < 128; i++) {\n")
        f.write("        float s = gnn_f24_pd4_b[i];\n")
        f.write("        for (j = 0; j < GNN_F24_H; j++) s += gnn_f24_pd4_w[i*GNN_F24_H+j] * x[j];\n")
        f.write("        m[i] = gnn_f24_gelu(s);\n")
        f.write("    }\n")
        f.write("    float logit = gnn_f24_pd6_b[0];\n")
        f.write("    for (j = 0; j < 128; j++) logit += gnn_f24_pd6_w[j] * m[j];\n\n")

        f.write("    return 1.0f / (1.0f + expf(-logit));\n")
        f.write("}\n\n")

        f.write("#endif /* GNN_FAN24_MODEL_H */\n")

    import os
    size_kb = os.path.getsize(out_path) / 1024
    n_params = sum(p.numel() for p in model.parameters())
    print(f"Exported: {out_path} ({size_kb:.0f} KB)", file=sys.stderr)
    print(f"Parameters: {n_params:,}", file=sys.stderr)

if __name__ == '__main__':
    main()
