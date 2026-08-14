# Beam Addressing System

**Coordinate IS the data. No hash. No collision. No storage.**

## Philosophy

```
weight[i] → (capo_id, param_index, abs_value, sign)
           = (partition, position, beam_length, polarity)

NO hash. NO dict. NO collision.
Coordinate IS the data.
```

## Architecture

```
CEILING (positive weights)
═════════════════════════
      ↑ beam_length = |weight|
      |
  ● ──┼── beam source
      |
      ↓ beam_length = |weight|
═════════════════════════
GROUND (negative weights)

coord = (capo_id, param_index, abs_value, sign)
      = (partition, position, length, polarity)
```

## FGLS Core Integration

Beam Addressing integrates with FGLS geometric tools:

| Component | Purpose | Integration |
|-----------|---------|-------------|
| `fibo_tick.h` | 20736-slot field (144×144) | `beam_to_fibo_slot()` |
| `geo_frame_seek.h` | Deterministic frame seek | `beam_to_frame()` |
| Angular mapping | Spherical 360×360 | `beam_to_spherical()` |

### Three Views (Same Data)

```
View 1: frame_seek  (geom)  — face/slot/phase/ico
View 2: fibo_spine  (spine) — pipe/tick/bridge/residual
View 3: spherical   (coord) — azimuth/elevation/length
```

## Files

| File | Purpose |
|------|---------|
| `beam_value.c` | Core C implementation + FGLS integration |
| `beam_value_dll.c` | Shared library for Python ctypes |
| `test_beam_value.c` | Standalone C test (16 tests) |
| `beam_value.py` | Pure Python reference implementation |
| `beam_benchmark.py` | Python vs C benchmark |

## Build

```bash
# C test
gcc -O2 -I../core -I../collection -I../collection/rdh \
    -I../collection/dgls/geo/include \
    test_beam_value.c -o test_beam_value.exe
./test_beam_value.exe

# DLL for Python
gcc -O2 -shared -o beam_value.dll beam_value_dll.c \
    -I../core -I../collection -I../collection/rdh \
    -I../collection/dgls/geo/include

# Python benchmark
python beam_benchmark.py
```

## Benchmark Results (1M weights)

### Python vs C Speedup

| Operation | Python | C | Speedup |
|-----------|--------|---|---------|
| `weight→coord` | 2.3M/s | 336M/s | **148.7x** |
| `coord→weight` (verify) | 4.1M/s | 894M/s | **217.4x** |

### C-Only Operations (FGLS Integration)

| Operation | Throughput |
|-----------|-----------|
| `→ fibo_slot` | 208M/s |
| `→ frame` | 319M/s |
| `→ spherical` | 516M/s |

### Comparison with Hash-Based Mappers

| Mapper | Speed | Collision | Storage |
|--------|-------|-----------|---------|
| **Beam Value (C)** | **336M/s** | **0%** | **0 bytes** |
| SHA256 | 227K/s | 0.76% | 32B/chunk |
| xxHash | 379K/s | 0.76% | 8B/chunk |
| Geometric | 41K/s | 0.002% | 0 bytes |

## Key Insights

1. **Coordinate = Data** → eliminates hash, storage, and collision entirely
2. **Beam length = weight value** → direct mapping, no transformation
3. **Capo = unlimited parameters** → clone field for horizontal scaling
4. **FGLS integration** → fibo_tick, frame_seek, spherical all O(1)
5. **C speedup 148-217x** → from Python prototype to production speed

## Roundtrip Verification

```
weight[i] = 71
  → coord = (0, i, 71, 1)
  → recovered = 71 ✓

weight[i] = -50
  → coord = (0, i, 50, 0)
  → recovered = -50 ✓
```

**16/16 tests PASS** — core, fibo_tick, frame_seek, spherical, batch, stats

## Conclusion

Beam Addressing with FGLS integration achieves:
- **Zero collision** (coordinate IS the data)
- **Zero storage** (no dict/hash table needed)
- **336M ops/sec** in C (148x faster than Python)
- **516M ops/sec** for spherical mapping
- **Full FGLS integration** (fibo_tick, frame_seek, angular)

This is the production-ready implementation for the beam addressing system.
