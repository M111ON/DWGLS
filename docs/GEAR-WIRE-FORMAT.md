# GHST+Gear-Wire Container Format — Interop Spec v1

**Date:** 2026-08-26  
**Status:** Implemented, oracle-passed, regression-tested  
**Header:** `core/gear_wire_bridge.h` (self-contained, no DWGLS deps)

---

## 1. Container Layout

All multi-byte integers are **little-endian**.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | ASCII `"GHST"` |
| 4 | 2 | version | `u16 = 1` |
| 6 | 2 | reserved | `u16 = 0` |
| 8 | 4 | count | `u32` — number of entries |
| 12 | count×5 | entries | packed entry records (see §2) |
| base | remaining | wire | gear event bytes + seals (see §3) |

`base = 12 + count × 5`. Wire region length = `file_size − base`.

---

## 2. Entry Record (5 bytes, packed)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 2 | block_id | `u16 LE` — block identifier |
| 2 | 1 | from_scale | `u8` — birth scale (envelope floor) |
| 3 | 1 | to_scale | `u8` — destination scale |
| 4 | 1 | flags | bitfield (see below) |

**Flags byte:**

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | LIFT | live route (frozen + tracked) |
| 1 | EXPIRED | tombstoned (audit trail) |
| 2 | DELTA | payload = pred+ent blob |
| 3 | GEAR | owns a wire byte (entry↔wire association) |
| 4–7 | — | reserved (0) |

Entries are **sorted** by `(block_id, from_scale)` — the b-bond principle.

---

## 3. Gear Wire

After the GHST entry table, the remaining bytes are the gear wire region.

### 3.1 Event Byte

Each non-seal byte encodes a scale transition as a gear event:

```
  7   6 5 4   3 2 1 0
  ┌───┬───────┬───────┐
  │ dx│  dc   │   q   │
  │ 2b│  3b   │  3b   │
  └───┴───────┴───────┘
```

- **q** (bits 0–2): full turns of the 24-tooth rim. Local field: q ∈ [0,5]; full field (FGF2): q ∈ [0,863].
- **dc** (bits 3–5): KIS cube-wheel remainder. dc = Δ % 8.
- **dx** (bits 6–7): hyperbolic axis-wheel remainder. dx = Δ % 3.

**Δ = q × 24 + CRT(dc, dx)** where CRT solves:

```
s ≡ dc (mod 8)
s ≡ dx (mod 3)
s = dc + 8 × ((2 × (dx − dc)) mod 3)    ∈ [0, 24)
```

**Bit7 is always 0** for valid events (dc ≤ 7 and dx ≤ 3 guarantees bit7 = dx>>1 ≤ 1; actually dx=3 → bit7=1? dx is 2 bits → dx ∈ {0,1,2,3}; dx=3 → bits 6–7 = 11 → bit7=1. But CRT produces s < 24, and Δ mod 24 = s, so d%24 ∈ [0,23]. For d=23: q=0, r=23 → dc=7, dx=2 (23%3=2). So dx never reaches 3. Thus bit7 is always 0 for Δ-mod-24 events. **This is why 0xFF is an unreachable seal value.**

### 3.2 Seal Byte

`0xFF` — chain terminator. Not a valid event (would require bit7=1 which Δ-mod-24 never produces).

A seal marks "no live hop beyond this point". When encountered during chain replay, the walk stops.

### 3.3 Ownership

The **k-th non-seal byte** in the wire belongs to the **k-th geared-flagged entry** (flag bit 3 set) in sorted entry order. A seal belongs to the block of the nearest preceding event.

### 3.4 Canonical Wire Order

On disk, wire bytes are grouped by **block id ascending**. Within a block, bytes appear in **append (call) order**. Seals sit at their call position within their block's run.

### 3.5 Seal-Accounting Invariant

```
non-seal wire bytes  ==  entries with flag bit 3 set
```

0..k seals are all valid. Containers without any expire (0 seals) are valid.

---

## 4. Chain Replay (Enter Anywhere)

The wire stores **Δ only** — no absolute scale anywhere. Chain replay needs the **reader's own birth position**.

### Forward Walk

```
w₀ = birth_scale
w₁ = (w₀ + Δ₁) % 144
w₂ = (w₁ + Δ₂) % 144
...
```

### Backward Reconstruct

Given the reader's current scale `cur_w` and n events:

```
out[n] = cur_w
out[i] = (out[i+1] − Δ_{i+1}) % 144    for i = n-1 .. 0
```

`out[0]` is the append scale. Any entry point reproduces the chain exactly.

---

## 5. Worked Example

**Transition:** from_scale=3 → to_scale=51

```
D = (51 + 144 − 3) % 144 = 48
q = 48 / 24 = 2
r = 48 % 24 = 0
dc = 0 % 8 = 0
dx = 0 % 3 = 0

Byte = 2 | (0 << 3) | (0 << 6) = 0x02

Verify: step(3, q=2, dc=0, dx=0) = (3 + 2×24 + CRT(0,0)) % 144
                                     = (3 + 48 + 0) % 144 = 51 ✓
```

---

## 6. Error Codes (bridge API)

| Code | Name | Meaning |
|------|------|---------|
| 0 | GWB_OK | success |
| −1 | GWB_E_BADARG | NULL argument |
| −2 | GWB_E_SMALL | buffer < 12 B |
| −3 | GWB_E_MAGIC | not "GHST" |
| −4 | GWB_E_VER | version ≠ 1 |
| −5 | GWB_E_COUNT | count > 4096 |
| −6 | GWB_E_TRUNC | entries past buffer |
| −7 | GWB_E_WIRE | seal accounting violated |

---

## 7. JSON Schema

The `gwb_json()` emitter produces:

```json
{
  "format": "ghst-gear-wire",
  "version": 1,
  "entries": 3,
  "geared": 3,
  "seals": 1,
  "valid": true,
  "records": [
    {"block": 3, "from": 5, "to": 70, "flags": "LG"}
  ],
  "wire": [
    {"block": 3, "event": {"q": 2, "dc": 1, "dx": 2}, "delta": 65},
    {"block": 9, "seal": true}
  ],
  "wire_bytes_hex": "8A9292FF"
}
```

Flags in records: `L`=lift, `E`=expired, `D`=delta, `G`=gear, `.`=other.

---

## 8. Constants

| Name | Value | Description |
|------|-------|-------------|
| GHOST_LOG_MAX | 4096 | max entries/wire bytes |
| GWB_SEAL | 0xFF | chain terminator |
| GWB_RING | 24 | rim teeth (8×3) |
| GWB_FIELD | 144 | local scale ring |
| GHOST_FLAG_GEAR | 0x08 | entry owns wire byte |
| GHOST_GEAR_SEAL_FROM | 256 | seal owner sentinel (from_scale=256) |

---

## 9. FGF2 Full-Field Container (Family B, 20736)

Magic ASCII `"FGF2"` on disk (bytes 0x46, 0x47, 0x46, 0x32).

### Header (12 bytes LE)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | ASCII `"FGF2"` |
| 4 | 4 | version | `u32 = 1` |
| 8 | 2 | n | `u16` — event count |
| 10 | 1 | rim_mode | `u8` — 0=FREE, 1=RIM |
| 11 | 1 | reserved | `u8 = 0` |

### Event packing (FREE mode, 2 bytes LE)

```
  15  14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
  ┌───┬─────┬─────┬──────────────────────────────────┐
  │spare│  dx │  dc │              q                   │
  │ 1b  │ 2b  │ 3b  │            10b                   │
  └───┴─────┴─────┴──────────────────────────────────┘
  byte 1 (high)          byte 0 (low)
```

- **q** (bits 0–9): full turns of the 24-tooth rim, range [0,863]
- **dc** (bits 10–12): cube-wheel remainder, range [0,7]
- **dx** (bits 13–14): axis-wheel remainder, range [0,3]
- **bit 15**: spare, always 0 for valid events

**Seal: 0xFFFF** (both bytes 0xFF). Unreachable: max valid = 863|(7<<10)|(3<<13) = 0x7F5F, bit15 always 0.

### RIM mode

Packed q:10b values. ceil(n×10/8) bytes total. Not yet implemented in the bridge.

### Field size

20736 = 144 × 144. CRT formula identical to local (gwb_crt).

### Hand-worked example

from=100, to=500:
```
D = (500 + 20736 − 100) % 20736 = 400
q = 400 / 24 = 16
r = 400 % 24 = 16
dc = 16 % 8 = 0
dx = 16 % 3 = 1

raw = 16 | (0 << 10) | (1 << 13) = 0x2010
lo = 0x10, hi = 0x20

Verify: step(100, q=16, dc=0, dx=1) = (100 + 16×24 + CRT(0,1)) % 20736
                                      = (100 + 384 + 16) % 20736 = 500 ✓
```
