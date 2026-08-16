---
luminaCreated: 2026-08-16T06:55:01.563Z
tags: []
luminaModified: 2026-08-16T06:55:01.563Z
luminaVersion: 1.3.11
---
# DWGLS → Geometric Filesystem: Capability Analysis

**Date:** 2026-08-06  
**Source:** 6 core headers + 3 supporting headers from DWGLS + FGLS_new

---

## Summary Table

| Header | Data Structure | Operation | FS Mapping |
|--------|---------------|-----------|------------|
| `geo_cube_in_dodeca.h` | Dodecahedron vertices, cube half-axes, cell types | φ-scaling address → 3D position | **Block addressing scheme** |
| `geo_cube_addr.h` | `GeoCubeAddr {gen, face, slot, cell_type, w_time}` | Flat ↔ (gen,face,slot) roundtrip | **Inode number ↔ path translation** |
| `geo_cell_addr.h` | `GeoCellAddr {gen, face, slot, cell_type}` | O(1) offset → (pipe_id, tick) | **Block → sector mapping** |
| `geo_adaptive_store.h` | `AdaptiveStore` (4 tiers, 768B–20.7KB) | Entropy-driven tier selection, CRC32 | **Adaptive extent allocation** |
| `frustum_gcfs.h` | 4896B GCFS file layout | Serialize/deserialize `FrustumStore` | **Sector serialization format** |
| `frustum_layout_v2.h` | Redirect only | Delegates to active_updates | **(not analyzed)** |

---

## Detailed Analysis

### 1. geo_cube_in_dodeca.h — Block Addressing Scheme

**Data Structures:**
- `Vec3D` — 3D vertex coordinates
- `DODECA_VERTS_TABLE[20]` — 20 dodecahedron vertices (8 cube + 12 golden rectangle)
- `HALF_AXIS_VERTS[3][2][4]` — 6 half-axes × 4 vertices each
- `CellType` enum — 8 cell types from 3-bit parity (III, IID, IDI, IDD, DII, DID, DDI, DDD)

**Operations:**
- `gen_scale(n)` — φⁿ growth factor per generation
- `cell_type_from_parity(nx, ny, nz)` — parity → cell classification
- `cube_address_to_xyz(n, k)` — address → 3D position
- `half_axis_center(axis, sign, n)` — face center at generation n
- `phi_ratio()` — verify φ relationship

**FS Mapping → Block Addressing:**
- **6 half-axes = 6 block "faces"** (X±, Y±, Z±). Each face is a block group.
- **Generation = block level** (0 = root blocks, 1 = sub-blocks, etc.)
- **φ-scaling = block size progression**: gen 0 = 1 block, gen 1 = 2 blocks, gen 2 = 3, gen 3 = 5, gen 4 = 8... (Fibonacci-like)
- **8 cell types = 8 block allocation policies**: III = pure data block, DDD = pure metadata block, IID = mixed data+meta, etc.
- **DiamondBlock = 64B sector** — the atomic unit across all faces

```
FS CONCEPT          GEOMETRY CONCEPT
─────────────────────────────────────
Block group         Half-axis (face 0-5)
Block level         Generation (0..31)
Sector              DiamondBlock (64B)
Allocation policy   CellType (3-bit parity)
Block size          gen_scale(n) = φⁿ
```

### 2. geo_cube_addr.h — Inode ↔ Path Translation

**Data Structure:**
```c
typedef struct {
    uint8_t  generation;   // which φ-layer (0..31)
    uint8_t  face;         // 0-5, which half-axis
    uint16_t slot;         // position within face
    uint8_t  cell_type;    // 0-7, auto-computed from parity
    double   w_time;       // temporal modulation (KIS timeline)
} GeoCubeAddr;
```

**Operations:**
- `geo_cube_addr(gen, face, slot)` — construct address
- `geo_cube_addr_to_flat(addr)` — address → flat index in [0, 20736)
- `geo_flat_to_addr(flat)` — flat index → address
- `geo_cube_addr_to_xyz(addr)` — address → 3D position
- `geo_cube_addr_w(addr, w_time)` — apply temporal modulation

**FS Mapping → Inode Number ↔ Path:**
- **GeoCubeAddr = inode path** (generation=directory depth, face=directory, slot=entry)
- **Flat index = inode number** (unique integer identifier in [0, 20736))
- **w_time = version/snapshot indicator** (same inode, different temporal state)
- **Roundtrip guarantee**: addr → flat → addr is lossless (proven)
- **Cell type = file type** (III=file, DDD=directory, IID=symlink, etc.)

```
FS CONCEPT          GEOMETRY CONCEPT
─────────────────────────────────────
Inode number        flat index (0..20735)
Inode path          GeoCubeAddr (gen, face, slot)
File type           CellType (3-bit parity)
Version/snapshot    w_time (temporal modulation)
Directory           face (6 faces = 6 top-level dirs)
```

### 3. geo_cell_addr.h — Block → Sector Mapping (Zero-Copy)

**Data Structure:**
```c
typedef struct {
    uint8_t  generation;   // 0..7
    uint8_t  face;         // 0..5
    uint16_t slot;         // 0..255
    uint8_t  cell_type;    // 3-bit parity
} GeoCellAddr;
```

**Operations:**
- `geo_cell_addr_from_offset(tensor_offset)` — O(1) flat → (gen, face, slot)
- `geo_cell_addr_to_pipe(addr, &pipe_id, &tick)` — address → rail coordinates
- `geo_cell_addr_offset_to_pipe(offset, &pipe_id, &tick)` — one-shot roundtrip

**The Zero-Copy Guarantee:**
```
Cell N in cube space = tick-(N/1728) on pipe (N%1728)
Same physical byte, two different views.
```

**Bit layout (14-bit flat cell id):**
```
bits 0-2   (3 bits) → generation (0-7)
bits 3-5   (3 bits) → face (0-5)
bits 6-13  (8 bits) → slot (0-255)
total = 14 bits → range [0, 16384) ⊂ [0, 20736)
```

**FS Mapping → Block → Sector:**
- **tensor_offset = logical block address (LBA)**
- **(gen, face, slot) = CHS (cylinder-head-sector)** — geometric equivalent of traditional disk addressing
- **(pipe_id, tick) = physical block address** — where the data actually lives
- **Zero-copy = same byte for both views** — no data movement between logical and physical
- **14-bit arithmetic = no loops, no malloc** — O(1) translation in hot path

```
FS CONCEPT          GEOMETRY CONCEPT
─────────────────────────────────────
LBA (logical)       tensor_offset
CHS (geometric)     (gen, face, slot)
Physical address    (pipe_id, tick)
Zero-copy           Cube ↔ Rail duality
Block translation   3 shifts + 3 masks
```

### 4. geo_adaptive_store.h — Adaptive Extent Allocation

**Data Structure:**
```c
typedef struct {
    uint16_t  enc;              // current frame enc (0..1439)
    uint8_t   tier;             // 0-3
    uint8_t   entropy_score;    // 0-255
    uint8_t   frame_count;      // 1, 3, 7, or 27
    uint16_t  frames[89];       // frame encs in range
    uint16_t  block_count;      // = frame_count × 12
    float     blocks[324 × 64]; // payload (max 20736 weights)
    uint32_t  total_weight_count;
    uint32_t  checksum;         // CRC32
} AdaptiveStore;
```

**Tier Mapping:**
| Tier | Entropy | Frames | Edges | Size | Use Case |
|------|---------|--------|-------|------|----------|
| 0 | 0-63 | 1 | 12 | 768B | Structured data (code, config) |
| 1 | 64-127 | 3 | 36 | 2.3KB | Moderate entropy (text, logs) |
| 2 | 128-191 | 7 | 84 | 5.4KB | High entropy (compressed data) |
| 3 | 192-255 | 27 | 324 | 20.7KB | Random data (encryption, noise) |

**Operations:**
- `adaptive_write(as, t, weight_buf, n, entropy_score)` — write with entropy-based sizing
- `adaptive_read(as, t, weight_buf, n)` — read back
- `adaptive_verify(as)` — CRC32 integrity check

**FS Mapping → Adaptive Extent Allocation:**
- **Tier = extent size policy** — allocate more blocks for high-entropy data
- **Entropy score = file fragmentation metric** — measure how "random" the data is
- **Frame range = extent span** — how many contiguous blocks to allocate
- **CRC32 = block checksum** — integrity verification
- **DiamondBlock (64B) = sector** — atomic allocation unit

```
FS CONCEPT          GEOMETRY CONCEPT
─────────────────────────────────────
Extent allocation   Tier selection (0-3)
Block count         Frame count × 12
Sector size         DiamondBlock (64B)
Integrity check     CRC32
Fragmentation       Entropy score (0-255)
```

### 5. frustum_gcfs.h — Sector Serialization Format

**File Layout (4896B = 288 × 17):**
```
[0    ..3455]  data zone     — 54 × 64B DiamondBlocks (verbatim copy)
[3456 ..3464]  coset_mask    — 9B (one byte per coset, reserved_mask[0..8])
[3465 ..3490]  letter_map    — 26B (A..Z, caller-supplied)
[3491 ..3498]  slop          — 8B (uint64_t last slope fingerprint)
[3499 ..3502]  merkle_root   — 4B (XOR of all slot core[0..3])
[3503 ..4895]  _pad          — 1393B zeros (boundary reserve)
```

**Key Insight:** 4896 = 2 × 3ⁿ × 17 — factor 17 ↔ FACE_PRIME {7,11,13,17,19,23}
- File boundary unreachable by pure 2×3ⁿ arithmetic (security seam)
- Metadata zone = 1440B = 2⁵ × 3² × 5 (intentional factor 5 marker)

**Operations:**
- `gcfs_serialize(FrustumStore *fs, uint8_t out[4896])` — store → file
- `gcfs_deserialize(FrustumStore *fs, const uint8_t in[4896])` — file → store
- `gcfs_merkle_verify(FrustumStore *fs)` — integrity check
- `gcfs_coset_summary(FrustumStore *fs, uint8_t coset_out[9])` — coset status

**Supporting Structures (from frustum_slot64.h + frustum_trit.h):**

```c
// FrustumSlot64 = 64B DiamondBlock
typedef struct {
    uint32_t  core[LEVEL_COUNT];  // 16B: merkle roots per level (0-3)
    uint16_t  reserved_mask;      // 2B: coset silence bitmap (9 bits)
    uint16_t  write_count;        // 2B: writes into this slot
    uint32_t  slope_lo;           // 4B: last slope fingerprint
    uint8_t   _pad[40];           // 40B: reserved
} FrustumSlot64;  // exactly 64B

// FrustumStore = 54 slots
typedef struct {
    FrustumSlot64 slots[GEAR_MESH];  // 54 × 64B = 3456B
    uint32_t      total_writes;
    uint32_t      total_silenced;
} FrustumStore;

// TritAddr = structural decomposition
typedef struct {
    uint8_t  trit;      // 0..26 — primary key
    uint8_t  coset;     // 0..8  — GiantCube zone (trit/3)
    uint8_t  face;      // 0..5  — cube direction (trit%6)
    uint8_t  level;     // 0..3  — core depth (trit%4)
    uint8_t  letter;    // 0..25 — LetterPair A..Z
    uint64_t slope;     // fibo_seed ^ addr — apex fingerprint
} TritAddr;
```

**FS Mapping → Sector Format:**
- **GCFS file = superblock + inode table** — the root metadata structure
- **54 DiamondBlocks = inode table** — 54 entries, each 64B
- **core[4] = 4-level B-tree** — merkle roots for hierarchical indexing
- **reserved_mask = deletion bitmap** — mark inodes as deleted without overwriting
- **letter_map = name lookup** — A..Z directory entries
- **merkle_root = filesystem integrity hash**
- **coset_mask = partition table** — 9 partitions (cosets) × 6 faces

```
FS CONCEPT          GEOMETRY CONCEPT
─────────────────────────────────────
Superblock          GCFS file header (4896B)
Inode table         54 × FrustumSlot64
B-tree levels       core[0..3] (4 levels)
Deletion bitmap     reserved_mask (9 cosets)
Name lookup         letter_map[26]
Integrity hash      merkle_root (4B XOR)
Partition table     coset_mask (9 bytes)
Slope fingerprint   Anti-tamper seal
```

---

## Mapping Summary: Geometry → FS Primitives

### Layer 1: Physical (geo_cube_in_dodeca.h)
```
Geometry:     Dodecahedron → Cube → 6 half-axes → DiamondBlock faces
FS Concept:   Disk → Platter → Sectors → Blocks
Unit:         DiamondBlock = 64B sector
```

### Layer 2: Logical (geo_cube_addr.h)
```
Geometry:     (generation, face, slot) → φ-scaled address space
FS Concept:   (directory depth, partition, entry) → inode path
Unit:         GeoCubeAddr = inode descriptor
Max:          20736 = 128 × 162 = 144 × 144 = 1728 × 12
```

### Layer 3: Translation (geo_cell_addr.h)
```
Geometry:     Flat offset ↔ Cube address ↔ Rail address
FS Concept:   LBA ↔ CHS ↔ Physical address
Unit:         14-bit flat cell id
Guarantee:    Zero-copy (same byte, two views)
```

### Layer 4: Allocation (geo_adaptive_store.h)
```
Geometry:     Entropy score → Tier → Frame range → Block count
FS Concept:   File properties → Extent size → Block allocation
Unit:         AdaptiveStore (768B to 20.7KB)
Policy:       Low entropy = small extent, high entropy = large extent
```

### Layer 5: Persistence (frustum_gcfs.h)
```
Geometry:     FrustumStore → 4896B GCFS file
FS Concept:   Inode table → Superblock on disk
Unit:         4896B = 2 × 3ⁿ × 17 (intentionally not 2×3-power)
Security:     File boundary unreachable by pure 2×3ⁿ arithmetic
```

---

## Key Constants (Sacred Number Chain)

```
20736 = 128 × 162 = 144 × 144 = 1728 × 12
       ↑           ↑           ↑          ↑
       Cube grid   Rail grid   Square     Pipes × Ticks

4896  = 54 × 64B + 1440B metadata
       ↑         ↑
       Frustum   Metadata zone (intentional factor 5)

54    = GEAR_MESH = 2 × 3³
       ↑
       Slots in FrustumStore (9 cosets × 6 faces)

64    = DIAMOND_BLOCK = 2⁶
       ↑
       Atomic sector size (all layers)
```

---

## Recommendations for Geometric Filesystem Design

1. **Reuse `geo_cell_addr.h`** for block → sector translation — it's O(1), zero-copy, and already proven (18/18 tests pass).

2. **Reuse `geo_cube_addr.h`** for inode addressing — the roundtrip (addr → flat → addr) is lossless and the φ-scaling gives natural directory hierarchy.

3. **Reuse `geo_adaptive_store.h`** for extent allocation — the 4-tier entropy-based sizing is perfect for files with varying content randomness.

4. **Reuse `frustum_gcfs.h`** for superblock format — the 4896B format with merkle integrity and coset partitioning is battle-tested.

5. **Extend `geo_cube_in_dodeca.h`** — add a `BlockType` enum mapping the 8 CellTypes to FS concepts (FILE, DIRECTORY, SYMLINK, DEVICE, etc.).

6. **New: `geo_fs_inode.h`** — combine GeoCubeAddr + FrustumSlot64 into a unified inode structure:
   ```c
   typedef struct {
       GeoCubeAddr    addr;        // path identity
       FrustumSlot64  slot;        // storage metadata
       uint32_t       size;        // file size in bytes
       uint16_t       block_start; // first DiamondBlock index
       uint16_t       block_count; // number of DiamondBlocks
       uint8_t        cell_type;   // file type (8 types)
       uint8_t        permissions; // rwx bits (8 bits)
   } GeoFsInode;  // ~96B per inode
   ```

7. **New: `geo_fs_superblock.h`** — combine GCFS format + adaptive store for root metadata:
   ```c
   typedef struct {
       GCubeFileHeader  header;       // 64B: magic, version, counts
       GeoFsInode       root_inode;   // 96B: root directory
       AdaptiveStore    journal;      // extent-based journal
       uint8_t          bitmap[256];  // block allocation bitmap (20736 bits)
       uint32_t         checksum;     // CRC32
   } GeoFsSuperblock;  // fits in 4896B GCFS format
   ```
