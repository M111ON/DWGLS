# Kis-Seal Research — Critical Findings
## Source: Research collaborator warning (Aug 2026)

---

## 🚨 MANDATORY RULES

### Rule 1: อย่าผูก physical storage เข้ากับ Rₙ ตรงๆ
```
ต้องแยก "scale" ออกจาก "shape"
ห้ามใช้ Rₙ = R₀(1+α)ⁿ จริงเป็นพิกัด storage
ถ้า n ใหญ่ → Rₙ ระเบิด (float overflow)
(1.382)^10000 = overflow ชัวร์
```

### Rule 2: n = abstract counter เท่านั้น
```
n = layer/generation index
n ∈ ℤ ไม่จำกัด
n เป็น integer counter ล้วนๆ
อย่าคำนวณ Rₙ จริงสำหรับ n ใหญ่

n ใช้สำหรับ:
✓ กันชน address ข้าม layer (anti-aliasing)
✗ คำนวณ radius จริง (float overflow)
```

### Rule 3: k = position within layer
```
k = 0-19 (icosa) หรือ 0-11 (dodeca)
k มี geometric adjacency
หน้าใกล้กันบนลูกบอล = weight ที่เกี่ยวข้องกัน

k ใช้สำหรับ:
✓ ตำแหน่งภายใน layer
✓ Geometric adjacency (compression/clustering)
✗ Raw byte addressing (ใช้ n แทน)
```

### Rule 4: shape = compression only
```
shape (icosa/dodeca) = ใช้ตอน compression/clustering
หน้าใกล้กันทางเรขาคณิต = weight ที่ควร cluster ด้วยกัน

shape ใช้สำหรับ:
✓ Compression/clustering
✓ DiamondBlock alignment
✓ Waveform Signature compression
✗ Raw addressing
```

---

## ⚠️ Pitfall: Alternating Capacity

```
Layer n=0: icosa  → 20 slots
Layer n=1: dodeca → 12 slots
Layer n=2: icosa  → 20 slots
Layer n=3: dodeca → 12 slots
...

ทุกจุดที่คำนวณ offset ต้อง:
if (n % 2 == 0) → 20 slots
else → 12 slots

ถ้าลืม branch นี้จุดเดียว:
- Address เลื่อนเพี้ยนทั้งระบบ
- ไม่มี error message
- แค่ผิดตำแหน่งเงียบๆ
- เหมือน pitfall ตอน derive dual solid
```

---

## ✅ What Works

### Direct O(1) offset formula
```
addr_to_byte_offset(n, k) = closed-form O(1)
- หาร-คูณตรงๆ
- ไม่มี loop, ไม่มี search
- offset ระหว่างชั้นสลับ 1280 / 768 byte (20×64 / 12×64)
- ไม่มี gap ไม่มี overlap
```

### Verified: random access element 737,412
```
→ address (n=46088, k=4)
→ byte offset ทันที ✅
```

---

## 💡 Geometric Locality = Compression Payoff

```
Icosahedron มี edge-adjacency structure
- หน้าไหนติดกันบนลูกบอล

ถ้า map weight tensor block ไปตาม k โดย:
- weight ที่คล้ายกัน (เช่น attention head ที่ correlate)
- อยู่ติดกันบน k

→ เวลาทำ DiamondBlock alignment / Waveform Signature compression
→ เจอ pattern ซ้ำในช่วง k ใกล้ๆ กันมากขึ้น

ใช้ function เดิม: nearest_pent(sphere) จาก GeoPixel
```

---

## 📋 Implementation Checklist

- [ ] แยก n (counter) ออกจาก Rₙ (radius)
- [ ] แยก k (position) ออกจาก shape (icosa/dodeca)
- [ ] ใช้ n เป็น abstract integer counter เท่านั้น
- [ ] ใช้ if/else สำหรับ alternating 20/12 slots
- [ ] ทดสอบ alternating offset ทุกจุด
- [ ] ใช้ geometric locality สำหรับ compression (ไม่ใช่ addressing)
