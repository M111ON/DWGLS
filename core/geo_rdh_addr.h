/* geo_rdh_addr.h — RDH (Ring-Wedge-Mirror) mixed-radix addressing for the ghost field
 * ═══════════════════════════════════════════════════════════════════════════
 * แทนที่ FNV-1a (pogls_fibo_addr) ในสาย ghost/bond ด้วย mixed-radix bijection
 * (จาก collection/rdh/rdh_addr.h — user research): coordinate IS the address.
 *
 *   ring  = block_id   (0..65535)   — กองข้อมูล (birth pile)
 *   wedge = from_scale (0..255)     — รอบเกิด (birth round — เสาเข็ม)
 *
 *   rdh_addr(block, from)   = block × 256 + from        (row-major encode)
 *   rdh_addr_twin(...)      = from × 65536 + block      (column-major)
 *   rdh_decompose(key)      → (block, from)             (REVERSIBLE)
 *
 * Guarantees:
 *   - collision-free BY CONSTRUCTION: mixed-radix encode เป็น bijection —
 *     พิสูจน์ได้ด้วยการ sweep ทั้ง 2^24 keys (test_rdh_addr)
 *   - reversible: decompose กู้ (block, from) กลับจาก address —
 *     "address IS data" ตรงตัว (ต่างจาก hash ที่ one-way)
 *   - no hash, no lookup table, int ล้วน — O(1) encode/decode
 *   - deterministic ข้าม session/platform/compiler
 *
 * Bond (L/R duality แบบไม่มี hash) — interleave:
 *   bond_L = rdh_addr(block, from)             (ครึ่งล่าง 24 bits)
 *   bond_R = rdh_addr(block, from) << 24       (ครึ่งบน 24 bits — ไม่ทับกัน)
 *   bond_key = bond_L XOR bond_R = addr | addr<<24  (bijection 48-bit)
 *
 *   ⚠️ L^R ของ row⊕column (L=addr, R=twin) ถูกพิสูจน์แล้วว่าไม่เป็น bijection
 *   (bits สัมพันธ์กัน → image เหลือ 2^16) — วัดจริงใน test_rdh_addr T1
 *   interleave ชนะ: สองครึ่งไม่ทับกัน → injective ตาม rdh_addr → collision-free
 *
 * สมบัติที่รักษา (ทำงานร่วมกับ ghost log):
 *   - from (birth round) อยู่ใน address → round ต่าง → bond ต่าง → เสาเข็มห้ามขยับ
 *   - to_scale ไม่อยู่ใน address (อยู่แค่ route ใน log) → telescope ได้
 *   - deterministic: (block, from) เดิม → address เดิมเสมอ
 *
 * NOTE: residual_space ตารางแคช (open addressing + probe) ยังใช้ _rs_hash
 * ตาม design ที่ยอมรับ (AGENTS.md — hash table สำหรับ cache) — RDH ใช้กับ
 * การหา ADDRESS (bond/geo_key) ซึ่งเป็นจุดที่เดิมใช้ FNV-1a
 */

#ifndef GEO_RDH_ADDR_H
#define GEO_RDH_ADDR_H

#include <stdint.h>

/* ── dimensions ─────────────────────────────────────────── */
#define RDH_N_RINGS   65536u   /* block space (uint16 block_id)   */
#define RDH_N_WEDGES  256u     /* scale space (uint8 from_scale)  */

/* mixed-radix encode — row-major: ring × n_wedges + wedge */
static inline uint64_t rdh_addr(uint32_t ring, uint32_t wedge) {
    return (uint64_t)ring * RDH_N_WEDGES + wedge;
}

/* mixed-radix encode — column-major twin: wedge × n_rings + ring
   ⚠️ L^R = addr ⊕ twin ถูกพิสูจน์ว่าไม่เป็น bijection (image เหลือ 2^16)
   เก็บไว้เป็น primitive สำหรับ duality อื่น (ไม่ใช้ใน bond) */
static inline uint64_t rdh_addr_twin(uint32_t ring, uint32_t wedge) {
    return (uint64_t)wedge * RDH_N_RINGS + ring;
}

/* REVERSIBLE — decompose address กลับเป็น (ring, wedge)
   "address IS data": กู้ coordinate กลับจาก address ได้ตรงๆ */
static inline void rdh_decompose(uint64_t key, uint32_t *ring, uint32_t *wedge) {
    *wedge = (uint32_t)(key % RDH_N_WEDGES);
    *ring  = (uint32_t)(key / RDH_N_WEDGES);
}

/* bond key — interleave: addr | addr<<24 (bijection 48-bit, deterministic)
   สองครึ่งเท่ากัน → decompose ครึ่งล่างได้ (address IS data) */
static inline uint64_t rdh_bond_key(uint32_t ring, uint32_t wedge) {
    uint64_t a = rdh_addr(ring, wedge);
    return a ^ (a << 24);
}

#endif /* GEO_RDH_ADDR_H */
