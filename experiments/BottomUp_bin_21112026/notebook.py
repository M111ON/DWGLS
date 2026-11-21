# /// script
# requires-python = ">=3.13"
# dependencies = [
#     "altair==6.3.0",
#     "duckdb==2.0.1",
#     "openai==3.57.0",
#     "polars[pyarrow]==2.1.0",
#     "pytest==9.2.0",
#     "sqlglot==30.33.0",
# ]
# ///

import marimo

__generated_with = "0.25.2"
app = marimo.App(width="medium", auto_download=["html"])


@app.cell
def _():
    import marimo as mo

    return (mo,)


@app.cell
def _():
    import numpy as np
    from PIL import Image

    class BitMatrix12_4:
        def __init__(self):
            # 1. โครงสร้างข้อมูลหลัก: 162 Anchors x 128 bits = 20,736 bits (2,592 bytes)
            self.num_anchors = 162
            self.bits_per_anchor = 128  # 96-bit Quadtree + 32-bit Vertical
            self.total_bits = self.num_anchors * self.bits_per_anchor  # 20,736
            self.grid_size = 144  # 144 x 144 = 20,736 = 12^4
        
            # Tile mapping for vertical 32 bits: 6x6 tiles (24x24 each), remove 4 corners = 32 tiles
            self.tile_grid = 6
            self.tile_size = self.grid_size // self.tile_grid  # 24
            self.corner_tiles = [(0, 0), (0, 5), (5, 0), (5, 5)]  # 4 มุม
            self.tile_positions = [(ty, tx) for ty in range(self.tile_grid) for tx in range(self.tile_grid)
                                   if (ty, tx) not in self.corner_tiles]  # 32 positions
            assert len(self.tile_positions) == 32
        
            # เก็บเป็น 1D Bit Array (ค่า 0 หรือ 1)
            self.buffer = np.zeros(self.total_bits, dtype=np.uint8)

        # ---------------------------------------------------------
        # 2. การแปลงพิกัด: Anchor <-> 1D Index <-> 2D (144x144) <-> 12^4
        # ---------------------------------------------------------
        def anchor_to_1d(self, anchor_idx: int, bit_offset: int) -> int:
            assert 0 <= anchor_idx < self.num_anchors
            assert 0 <= bit_offset < self.bits_per_anchor
            return (anchor_idx * self.bits_per_anchor) + bit_offset

        def index_1d_to_2d(self, idx_1d: int) -> tuple[int, int]:
            x = idx_1d % self.grid_size
            y = idx_1d // self.grid_size
            return x, y

        def index_1d_to_12_4(self, idx_1d: int) -> tuple[int, int, int, int]:
            """แปลงตำแหน่ง 1D (0..20735) เป็นพิกัดต้นไม้ 4 ชั้น (d0, d1, d2, d3) แต่ละชั้นมีค่า 0..11"""
            d3 = idx_1d % 12
            d2 = (idx_1d // 12) % 12
            d1 = (idx_1d // 144) % 12
            d0 = (idx_1d // 1728) % 12
            return (d0, d1, d2, d3)

        # ---------------------------------------------------------
        # 3. ระบบ Quadtree (96 bits) + Bottom-Up Cascade & Validation
        # ---------------------------------------------------------
        def set_anchor_data(self, anchor_idx: int, quad_leaf_64: np.ndarray, vertical_32: np.ndarray = None):
            """
            โครงสร้าง 96 bits ของ Quadtree:
            - ชั้นล่างสุด (Leaf): 64 bits (bit 32..95)
            - ชั้นกลาง: 16 bits (bit 16..31) -> ยุบจาก 64 (กลุ่มละ 4) ด้วย OR
            - ชั้นบน: 4 bits (bit 12..15) -> ยุบจาก 16 (กลุ่มละ 4) ด้วย OR
            - ชั้นราก: 1 bit (bit 11) -> ยุบจาก 4 (กลุ่มละ 4) ด้วย OR
            - (bit 0..10 สำรองไว้ใช้เป็น Header/Flags)
        
            vertical_32: ถ้าไม่ระบุ จะ auto-calculate จาก quadtree activity (96 bits)
            """
            assert len(quad_leaf_64) == 64

            # Bottom-Up Evaluation (เปิดชั้นบนอัตโนมัติด้วย Bitwise OR)
            lvl_16 = np.bitwise_or.reduce(quad_leaf_64.reshape(16, 4), axis=1)
            lvl_4  = np.bitwise_or.reduce(lvl_16.reshape(4, 4), axis=1)
            lvl_1  = np.array([np.bitwise_or.reduce(lvl_4)], dtype=np.uint8)

            # ประกอบร่าง 96 bits
            quad_96 = np.zeros(96, dtype=np.uint8)
            quad_96[11] = lvl_1[0]
            quad_96[12:16] = lvl_4
            quad_96[16:32] = lvl_16
            quad_96[32:96] = quad_leaf_64

            # Auto-calculate vertical_32 จาก quadtree 96-bit activity
            if vertical_32 is None:
                vertical_32 = self._auto_vertical_from_quadtree(quad_96)
            else:
                assert len(vertical_32) == 32

            # บันทึกลง Buffer หลัก (96 bits + 32 bits)
            start = anchor_idx * self.bits_per_anchor
            self.buffer[start : start + 96] = quad_96
            self.buffer[start + 96 : start + 128] = vertical_32

        def _auto_vertical_from_quadtree(self, quad_96: np.ndarray) -> np.ndarray:
            """
            Auto-generate 32 vertical bits from 96-bit quadtree activity.
            กติกา: vertical bit จะ active ถ้า corresponding region ใน quadtree มี activity
            - แบ่ง 96 bits เป็น 32 กลุ่ม กลุ่มละ 3 bits
            - แต่ละ vertical bit = OR ของ 3 quadtree bits ในกลุ่มนั้น
            - เพิ่ม randomness เล็กน้อยเพื่อ variation
            """
            vertical_32 = np.zeros(32, dtype=np.uint8)
        
            # ใช้ leaf bits (64 bits) + upper bits (32 bits) = 96 bits
            # แบ่ง 96 bits เป็น 32 กลุ่ม กลุ่มละ 3 bits
            for i in range(32):
                group_start = i * 3
                group_end = min(group_start + 3, 96)
                # Vertical bit = OR ของกลุ่ม quadtree bits
                vertical_32[i] = np.bitwise_or.reduce(quad_96[group_start:group_end])
        
            # ถ้าทุก bit เป็น 0 (ไม่มี activity เลย) ให้สุ่มเปิด 1-2 bits
            if np.sum(vertical_32) == 0:
                n = np.random.randint(1, 3)
                vertical_32[np.random.choice(32, size=n, replace=False)] = 1
        
            return vertical_32

        def validate_anchor(self, anchor_idx: int) -> bool:
            """ตรวจสอบกฎ: หากชั้นล่างเปิด >= 1/4 (16 bits), ชั้นบนและแนวตั้ง 32-bit ต้อง Active ตามเกณฑ์"""
            start = anchor_idx * self.bits_per_anchor
            quad_96 = self.buffer[start : start + 96]
            vertical_32 = self.buffer[start + 96 : start + 128]

            leaf_active = np.sum(quad_96[32:96])
            upper_active = np.sum(quad_96[11:32])
            vert_active = np.sum(vertical_32)

            # กฎ: ถ้าชั้นล่างเปิด >= 1/4 (16 ช่อง)
            if leaf_active >= 16:
                # ชั้นกลางต้องเปิดอย่างน้อย 4 ช่อง (16/4) และแนวตั้ง 32-bit ต้องไม่เป็น 0
                if upper_active < 6 or vert_active == 0:
                    return False
            return True

        # ---------------------------------------------------------
        # 4. การแปลงเป็น 2D Pixel (144x144) และบันทึกเป็น 1-bit PNG
        # ---------------------------------------------------------
        def to_2d_matrix(self) -> np.ndarray:
            """Original flat reshape - all anchors"""
            return self.buffer.reshape((self.grid_size, self.grid_size))

        def to_anchor_tile_matrix(self, anchor_idx: int = 0) -> np.ndarray:
            """
            สร้าง 144x144 RGB matrix สำหรับ anchor เดียวด้วย Tile Layout:
            - 6x6 tiles (24x24 px each), remove 4 corners = 32 tiles
            - 96 quadtree bits -> วางกระจายใน 32 tiles (สีขาว/เทา)
            - 32 vertical bits -> วางที่มุมซ้ายบน (0,0) ของแต่ละ tile 32 tiles 
              * Active = Cyan (สีฟ้าเขียว)
              * Inactive = Dark Red (สีแดงเข้ม)
            - 4 Corner tiles = สีเหลือง (markers)
            - คืนค่าเป็น (144, 144, 3) uint8 RGB
            """
            start = anchor_idx * self.bits_per_anchor
            quad_96 = self.buffer[start : start + 96]
            vertical_32 = self.buffer[start + 96 : start + 128]
        
            # สร้าง RGB canvas (พื้นหลังดำ)
            rgb = np.zeros((self.grid_size, self.grid_size, 3), dtype=np.uint8)
        
            # ============================================================
            # 1. วาง 96 Quadtree bits ใน 32 non-corner tiles (3 bits ต่อ tile)
            # ============================================================
            quad_idx = 0
            for ty in range(self.tile_grid):
                for tx in range(self.tile_grid):
                    if (ty, tx) in self.corner_tiles:
                        continue  # ข้าม 4 มุม
                
                    y0, x0 = ty * self.tile_size, tx * self.tile_size
                    # แต่ละ tile 24x24 วาง 3 bits แนวตั้ง (ห่างกัน 8 px)
                    for i in range(3):
                        if quad_idx >= 96:
                            break
                        py = y0 + i * 8
                        px = x0 + 2
                        if py < self.grid_size and px < self.grid_size:
                            val = quad_96[quad_idx] * 200  # 0 หรือ 200 (ขาว/เทา)
                            rgb[py, px] = [val, val, val]
                        quad_idx += 1
        
            # ============================================================
            # 2. วาง 32 Vertical bits ที่ช่องแรก (top-left) ของแต่ละ 32 tiles
            # ============================================================
            for v_idx, (ty, tx) in enumerate(self.tile_positions):
                y0, x0 = ty * self.tile_size, tx * self.tile_size
                val = vertical_32[v_idx]
            
                if val == 1:
                    # Active: Cyan (สีฟ้าเขียวสว่าง) - วาด 3x3 block เพื่อเห็นชัด
                    color = [0, 255, 255]  # Cyan
                else:
                    # Inactive: Dark Red (สีแดงเข้ม) - วาด 3x3 block
                    color = [180, 0, 0]  # Dark Red
            
                # วาดที่มุมซ้ายบนของ tile (3x3 pixels)
                for dy in range(3):
                    for dx in range(3):
                        py, px = y0 + dy, x0 + dx
                        if py < self.grid_size and px < self.grid_size:
                            rgb[py, px] = color
        
            # ============================================================
            # 3. Mark 4 Corner tiles ด้วยสีเหลือง (ขอบ 3px)
            # ============================================================
            for ty, tx in self.corner_tiles:
                y0, x0 = ty * self.tile_size, tx * self.tile_size
                # Top edge
                rgb[y0:y0+3, x0:x0+self.tile_size] = [255, 255, 0]
                # Left edge
                rgb[y0:y0+self.tile_size, x0:x0+3] = [255, 255, 0]
                # Bottom edge (ถ้าเป็น bottom row)
                if ty == 5:
                    rgb[y0+self.tile_size-3:y0+self.tile_size, x0:x0+self.tile_size] = [255, 255, 0]
                # Right edge (ถ้าเป็น right column)
                if tx == 5:
                    rgb[y0:y0+self.tile_size, x0+self.tile_size-3:x0+self.tile_size] = [255, 255, 0]
        
            # ============================================================
            # 4. วาด Grid lines ขาวอ่อนระหว่าง tiles
            # ============================================================
            for i in range(1, self.tile_grid):
                # Horizontal lines
                rgb[i*self.tile_size-1:i*self.tile_size+1, :] = [50, 50, 50]
                # Vertical lines
                rgb[:, i*self.tile_size-1:i*self.tile_size+1] = [50, 50, 50]
        
            return rgb

        def export_1bit_png(self, filename: str = "matrix_144x144.png"):
            matrix_2d = self.to_2d_matrix()
            img = Image.fromarray((matrix_2d * 255).astype(np.uint8), mode='L').convert('1')
            img.save(filename)
            print(f"บันทึกภาพ {filename} สำเร็จ (ขนาด 144x144, Raw Data = {self.total_bits // 8} bytes)")

        def export_tile_png(self, anchor_idx: int, filename: str = "anchor_tile_144x144.png"):
            """Export tile visualization as RGB PNG"""
            rgb = self.to_anchor_tile_matrix(anchor_idx)
            img = Image.fromarray(rgb, mode='RGB')
            img.save(filename)
            print(f"บันทึก Tile Visualization {filename} สำเร็จ (144x144 RGB)")

        def import_1bit_png(self, filename: str):
            img = Image.open(filename).convert('1')
            assert img.size == (self.grid_size, self.grid_size), "ขนาดภาพต้องเป็น 144x144 เท่านั้น"
            arr = np.array(img, dtype=np.uint8)
            self.buffer = arr.flatten()


    # =========================================================
    # ตัวอย่างการทดสอบรันระบบ
    # =========================================================
    if __name__ == "__main__":
        bm = BitMatrix12_4()

        # จำลองข้อมูล Anchor ที่ 0: เปิดชั้นล่างสุด 1/4 (16 จาก 64 ช่อง)
        leaf = np.zeros(64, dtype=np.uint8)
        leaf[0:16] = 1  # เปิด 16 ช่องแรก

        # ไม่ระบุ vertical_32 ให้ auto-calculate จาก quadtree activity
        bm.set_anchor_data(anchor_idx=0, quad_leaf_64=leaf)

        # ตรวจสอบความถูกต้องตามกฎ
        is_valid = bm.validate_anchor(anchor_idx=0)
        print(f"Anchor 0 ผ่านการตรวจสอบกฎหรือไม่: {is_valid}")

        # ดูการแปลงพิกัดไปเป็น 12^4 ของบิตที่ 32 (จุดเริ่มของ Leaf)
        idx = bm.anchor_to_1d(anchor_idx=0, bit_offset=32)
        print(f"Bit 32 -> 2D Pixel: {bm.index_1d_to_2d(idx)} | พิกัด 12^4: {bm.index_1d_to_12_4(idx)}")

        # บันทึกเป็นไฟล์ภาพขาว-ดำ 144x144 (raw buffer)
        bm.export_1bit_png("test_12_4.png")
    
        # บันทึก Tile Visualization
        bm.export_tile_png(0, "test_tile_144x144.png")
    return BitMatrix12_4, bm, np


@app.cell
def _():
    # State ที่คงอยู่ข้ามการ re-run (cell นี้ไม่มี dependency reactive จึงไม่ถูกสร้างใหม่)
    class AnchorState:
        def __init__(self) -> None:
            self.n_active = 0
            self.last_slider = None
            self.last_action = "เริ่มต้น: ยังไม่ได้สุ่ม — เลื่อน Slider หรือกดปุ่ม 🎲 ด้านล่าง"

    state = AnchorState()
    return (state,)


@app.cell
def _(bm, n_random, np, randomize_btn, state):
    # สุ่มเปิดจุดที่ชั้นล่างสุด (Leaf) — ชั้นที่สูงกว่าจะ cascade ขึ้นมาอัตโนมัติ
    # (set_anchor_data คำนวณ L16/L4/Root ด้วย Bitwise OR ให้อัตโนมัติ)
    def apply_active(n_leaf: int) -> None:
        """สุ่มจุด n_leaf จุดที่ชั้นล่างสุด แล้วให้ระบบคำนวณชั้นบนอัตโนมัติ"""
        new_leaf = np.zeros(64, dtype=np.uint8)
        new_leaf[np.random.choice(64, size=n_leaf, replace=False)] = 1

        # Vertical 32 bits: เปิดตามสัดส่วน (อย่างน้อย 1 ช่อง)
        new_vert = np.zeros(32, dtype=np.uint8)
        new_vert[np.random.choice(32, size=max(1, min(32, n_leaf // 4)), replace=False)] = 1

        bm.set_anchor_data(anchor_idx=0, quad_leaf_64=new_leaf, vertical_32=new_vert)
        state.n_active = n_leaf


    if randomize_btn.value:
        # ปุ่มสุ่ม: สุ่มจำนวนจุดเอง (8..64)
        n = int(np.random.randint(8, 65))
        apply_active(n)
        state.last_action = (
            f"🎲 สุ่มใหม่: Leaf active **{n}/64** จุด (จำนวนสุ่ม) — "
            f"ชั้นบน L16/L4/Root cascade ขึ้นมาอัตโนมัติ "
            f"(ตรวจสอบกฎ: {'ผ่าน' if bm.validate_anchor(0) else 'ไม่ผ่าน'})"
        )
    elif state.last_slider != n_random.value:
        # Slider: กำหนดจำนวนจุดตรงๆ
        state.last_slider = n_random.value
        apply_active(int(n_random.value))
        state.last_action = (
            f"จาก Slider: Leaf active **{n_random.value}/64** จุด — "
            f"ชั้นบน L16/L4/Root cascade ขึ้นมาอัตโนมัติ "
            f"(ตรวจสอบกฎ: {'ผ่าน' if bm.validate_anchor(0) else 'ไม่ผ่าน'})"
        )

    # ดึง bits หลัง cascade มาใช้สำหรับ plot
    quad_96 = bm.buffer[0:96]
    leaf_bits = quad_96[32:96]
    lvl16_bits = quad_96[16:32]
    lvl4_bits = quad_96[12:16]
    root_bits = quad_96[11:12]
    vert_bits = bm.buffer[96:128]
    status_text = f"{state.last_action} — Vertical active: {int(vert_bits.sum())}/32"
    return leaf_bits, lvl16_bits, lvl4_bits, root_bits, status_text


@app.cell
def _(leaf_bits, lvl16_bits, lvl4_bits, np, root_bits):
    import matplotlib.pyplot as plt

    # ── Plot 1: Quadtree Bottom-Up Cascade graph (Anchor 0) ──
    leaf_x = np.arange(64)
    lvl16_x = np.arange(16) * 4 + 3
    lvl4_x = np.arange(4) * 12 + 15
    root_x = np.array([31.5])
    tree_ys = [0, 1, 2, 3]

    tree_layers = [
        ("Leaf (64)\n(bottom)", leaf_x, leaf_bits, 55, 20),
        ("Level 16", lvl16_x, lvl16_bits, 55, 20),
        ("Level 4", lvl4_x, lvl4_bits, 70, 26),
        ("Root (1)", root_x, root_bits, 130, 50),
    ]

    fig_tree, ax = plt.subplots(figsize=(12, 4.8))


    def draw_edges(children, parents, y0, y1, color, lw):
        """Draw lines child -> parent (OR cascade)"""
        for parent, group in zip(parents, np.array_split(children, len(parents))):
            for child in group:
                ax.plot([child, parent], [y0, y1], color=color, lw=lw, zorder=1)


    draw_edges(leaf_x, lvl16_x, 0, 1, "0.85", 0.5)
    draw_edges(lvl16_x, lvl4_x, 1, 2, "0.72", 0.7)
    draw_edges(lvl4_x, root_x, 2, 3, "0.55", 0.9)

    # Nodes: active = red, inactive = gray
    for (name, x_arr, bits, s_on, s_off), y in zip(tree_layers, tree_ys):
        ax.scatter(
            x_arr,
            [y] * len(x_arr),
            c=np.where(bits == 1, "tab:red", "0.85"),
            s=np.where(bits == 1, s_on, s_off),
            zorder=2,
        )
        ax.text(-4, y, name, va="center", fontsize=9)

    ax.set_xlim(-24, 67)
    ax.set_ylim(-0.5, 3.5)
    ax.set_xticks([])
    ax.set_yticks([])
    for spine in ax.spines.values():
        spine.set_visible(False)

    ax.set_title(
        f"Bottom-Up Cascade (Anchor 0) — Leaf {int(leaf_bits.sum())}/64 | "
        f"L16 {int(lvl16_bits.sum())}/16 | L4 {int(lvl4_bits.sum())}/4 | Root {int(root_bits[0])}",
        loc="left",
        fontsize=11,
    )
    return fig_tree, plt


@app.cell
def _(bm, plt):
    # ── Plot 2: 144x144 Tile Visualization (6x6 tiles, 32 vertical bits at tile positions) ──
    tile_rgb = bm.to_anchor_tile_matrix(0)

    fig_matrix, (ax_full, ax_zoom) = plt.subplots(
        1, 2, figsize=(12, 4.8), gridspec_kw={"width_ratios": [2.0, 1.0]}
    )

    ax_full.imshow(tile_rgb, interpolation="nearest", aspect="equal")
    ax_full.set_title("Tile View 144x144 (6×6 tiles) — Cyan=Vert Active, Red=Vert Inactive, Gray=Quadtree", loc="left", fontsize=11)
    ax_full.set_xlabel("x (0..143)")
    ax_full.set_ylabel("y (0..143)")

    # Draw tile grid lines
    for i in range(1, 6):
        ax_full.axhline(i * 24 - 0.5, color='white', linewidth=0.5, alpha=0.3)
        ax_full.axvline(i * 24 - 0.5, color='white', linewidth=0.5, alpha=0.3)

    # Mark corner tiles
    for ty, tx in [(0,0), (0,5), (5,0), (5,5)]:
        ax_full.add_patch(plt.Rectangle((tx*24, ty*24), 24, 24, fill=False, edgecolor='yellow', linewidth=2, linestyle='--'))

    # Zoom: Show first tile row (top 24 pixels) with vertical bit positions
    tile_row = tile_rgb[0:24, :, :]
    ax_zoom.imshow(tile_row, interpolation="nearest", aspect="auto")
    ax_zoom.set_title("Top Tile Row (y=0..23) — Vertical bits at tile (0,0) positions", loc="left", fontsize=11)
    ax_zoom.set_yticks([0, 11, 23])
    ax_zoom.set_yticklabels(["Tile y=0", "Mid", "Tile y=23"])
    # Mark vertical bit positions (every 24px, skip corners at x=0 and x=120)
    for tx in range(6):
        if tx == 0 or tx == 5:
            continue  # corners
        ax_zoom.axvline(tx * 24, color='cyan', linewidth=1, alpha=0.5)

    plt.tight_layout()
    return (fig_matrix,)


@app.cell
def _(mo):
    # ── UI ควบคุม (ต้องอยู่ก่อนเซลล์ที่ใช้ .value) ──
    # เลื่อน Slider = กำหนดจำนวนจุดที่ชั้นล่างสุดตรงๆ
    # กดปุ่ม 🎲 = สุ่มจำนวนจุด (8..64) และสุ่มตำแหน่งที่ชั้นล่างสุด
    # การ active ชั้นล่างสุดจะทำให้ชั้นบน (L16/L4/Root) active ตามอัตโนมัติผ่าน Bitwise OR
    n_random = mo.ui.slider(8, 64, value=32, step=1, label="จำนวนจุด active (ชั้นล่างสุด)")
    randomize_btn = mo.ui.button(label="🎲 สุ่มจำนวนจุด active", kind="success")
    mo.hstack([n_random, randomize_btn])
    return n_random, randomize_btn


@app.cell
def _(fig_matrix, fig_tree, mo, status_text):
    mo.vstack(
        [
            mo.md(f"**{status_text}**"),
            fig_tree,
            fig_matrix,
        ]
    )
    return


@app.cell
def _(BitMatrix12_4, np):
    # ============================================================
    # 5. ฟังก์ชันสำหรับจำลองและจัดการ 162 Anchors ทั้งหมด
    # ============================================================

    def populate_all_anchors_162(bm: BitMatrix12_4, seed: int = 42, activity_range: tuple = (8, 48)):
        """
        เติมข้อมูลสุ่มให้ทุก 162 anchors
        - leaf_active: จำนวนจุด active ที่ชั้นล่างสุด (64 bits)
        - vertical bits คำนวณอัตโนมัติจาก quadtree activity
        - ใช้ seed เพื่อให้ reproducible
        """
        np.random.seed(seed)
        stats = []
    
        for anchor_idx in range(bm.num_anchors):
            n_leaf = np.random.randint(activity_range[0], activity_range[1] + 1)
        
            leaf = np.zeros(64, dtype=np.uint8)
            active_positions = np.random.choice(64, size=n_leaf, replace=False)
            leaf[active_positions] = 1
        
            bm.set_anchor_data(anchor_idx=anchor_idx, quad_leaf_64=leaf)
        
            quad_96 = bm.buffer[anchor_idx * 128 : anchor_idx * 128 + 96]
            vert_32 = bm.buffer[anchor_idx * 128 + 96 : anchor_idx * 128 + 128]
        
            leaf_count = int(np.sum(quad_96[32:96]))
            l16_count = int(np.sum(quad_96[16:32]))
            l4_count = int(np.sum(quad_96[12:16]))
            root_val = int(quad_96[11])
            vert_count = int(np.sum(vert_32))
            is_valid = bm.validate_anchor(anchor_idx)
        
            stats.append({
                'anchor': anchor_idx,
                'leaf_active': leaf_count,
                'l16_active': l16_count,
                'l4_active': l4_count,
                'root': root_val,
                'vert_active': vert_count,
                'valid': is_valid
            })
    
        return stats


    def get_anchor_summary_table_162(stats: list) -> str:
        """สร้างตารางสรุปสถิติของทุก anchors ในรูปแบบ Markdown"""
        total_leaf = sum(s['leaf_active'] for s in stats)
        total_vert = sum(s['vert_active'] for s in stats)
        valid_count = sum(1 for s in stats if s['valid'])
    
        ranges = [(0, 15), (16, 31), (32, 47), (48, 64)]
        range_counts = {f"{r[0]}-{r[1]}": 0 for r in ranges}
        for s in stats:
            for r in ranges:
                if r[0] <= s['leaf_active'] <= r[1]:
                    range_counts[f"{r[0]}-{r[1]}"] += 1
                    break
    
        md = f"""## 📊 สรุป 162 Anchors (96+32 bits × 162 = 20,736 bits)

    **ภาพรวม:**
    - Total Anchors: **162**
    - Valid Anchors: **{valid_count}/162** ({valid_count/162*100:.1f}%)
    - Total Leaf Active: **{total_leaf}/10,368** ({total_leaf/10368*100:.1f}%)
    - Total Vertical Active: **{total_vert}/5,184** ({total_vert/5184*100:.1f}%)
    - Avg Leaf/Anchor: **{total_leaf/162:.1f}**
    - Avg Vert/Anchor: **{total_vert/162:.1f}**

    **การกระจาย Leaf Activity:**
    """
        for r, c in range_counts.items():
            md += f"- {r}: **{c}** anchors ({c/162*100:.1f}%)\n"
    
        md += "\n**ตัวอย่าง 10 Anchors แรก:**\n\n"
        md += "| Anchor | Leaf | L16 | L4 | Root | Vert | Valid |\n"
        md += "|--------|------|-----|----|------|------|-------|\n"
        for s in stats[:10]:
            md += f"| {s['anchor']:3d} | {s['leaf_active']:4d} | {s['l16_active']:3d} | {s['l4_active']:2d} | {s['root']:4d} | {s['vert_active']:4d} | {'✅' if s['valid'] else '❌'} |\n"
    
        md += "\n... (แสดง 10 ตัวแรกจาก 162 anchors)\n"
        return md


    def create_full_matrix_view_162(bm: BitMatrix12_4) -> np.ndarray:
        """สร้างมุมมอง 144x144 ของข้อมูลทั้งหมด (162 anchors × 128 bits)"""
        return bm.to_2d_matrix()


    def create_anchor_grid_view_162(bm: BitMatrix12_4, anchors_per_row: int = 18) -> np.ndarray:
        """
        สร้างภาพรวมแบบ grid: แต่ละ anchor แสดงเป็น mini tile 8x8 pixels
        162 anchors = 9 rows × 18 cols = 162 tiles
        คืนค่า RGB image (72 x 144 x 3)
        """
        rows = (bm.num_anchors + anchors_per_row - 1) // anchors_per_row
        cols = anchors_per_row
        tile_h, tile_w = 8, 8
    
        grid_img = np.zeros((rows * tile_h, cols * tile_w, 3), dtype=np.uint8)
    
        for anchor_idx in range(bm.num_anchors):
            row = anchor_idx // cols
            col = anchor_idx % cols
        
            start = anchor_idx * bm.bits_per_anchor
            quad_96 = bm.buffer[start : start + 96]
            vert_32 = bm.buffer[start + 96 : start + 128]
        
            leaf_active = np.sum(quad_96[32:96]) / 64.0
            vert_active = np.sum(vert_32) / 32.0
        
            y0, x0 = row * tile_h, col * tile_w
        
            color = [
                int(vert_active * 255),
                int(leaf_active * 255),
                50
            ]
            grid_img[y0:y0+tile_h, x0:x0+tile_w] = color
        
            grid_img[y0:y0+tile_h, x0] = [255, 255, 255]
            grid_img[y0, x0:x0+tile_w] = [255, 255, 255]
    
        return grid_img

    print("✅ ฟังก์ชันสำหรับ 162 Anchors พร้อมใช้งาน")
    return get_anchor_summary_table_162, populate_all_anchors_162


@app.cell
def _(
    BitMatrix12_4,
    get_anchor_summary_table_162,
    mo,
    populate_all_anchors_162,
):
    # ============================================================
    # 6. สร้างข้อมูลจำลองสำหรับ 162 Anchors และแสดงผลสรุป
    # ============================================================

    # สร้าง instance ใหม่สำหรับ full simulation (แยกจาก bm เดิมที่ใช้กับ UI)
    bm_full = BitMatrix12_4()

    # Populate ทุก anchors (ใช้ seed เพื่อ reproducibility)
    stats_full = populate_all_anchors_162(bm_full, seed=42, activity_range=(8, 48))

    # แสดงตารางสรุป
    mo.md(get_anchor_summary_table_162(stats_full))
    return


if __name__ == "__main__":
    app.run()
