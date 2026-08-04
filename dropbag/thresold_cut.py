import numpy as np

# ค่าคงที่จากเอกสารวิจัย
E_MAX_L5 = 5.04   # Expansion Factor ที่ Layer 5 (อันตราย)
E_MAX_L0 = 1.00   # Expansion Factor ที่ Layer 0 (ปลอดภัย/Compressed)
SAFETY   = 1.5    # Headroom

def simulate_bounded_growth(data_gb, important_ratio=0.10, threshold=0.8):
    """
    จำลองการใช้ Threshold/Cardioid เพื่อจำกัดการขยายตัว
    - important_ratio: สัดส่วนข้อมูลที่ได้สิทธิไป L5 (หัวของ Cardioid)
    - ส่วนที่เหลือจะถูกดักไว้ที่ L0 (หางของ Cardioid)
    """
    
    # 1. คำนวณแบบ Unbounded (ปล่อยอิสระทุกจุดไป L5) -> เสี่ยง OOM
    # สูตร: Data * E_MAX_L5 * Safety
    mem_unbounded = data_gb * E_MAX_L5 * SAFETY
    
    # 2. คำนวณแบบ Bounded (ใช้ Threshold คัดกรอง)
    # ส่วนสำคัญ (10%) ไป L5, ส่วนไม่สำคัญ (90%) Stay at L0
    weighted_exp = (important_ratio * E_MAX_L5) + ((1 - important_ratio) * E_MAX_L0)
    mem_bounded = data_gb * weighted_exp * SAFETY
    
    # สถานะ
    ram_limit = 8.0 # สมมติ RAM 8GB
    status_unbounded = "OOM 💥" if mem_unbounded > ram_limit else "OK ✅"
    status_bounded   = "OOM 💥" if mem_bounded > ram_limit else "OK ✅"
    
    print(f"--- Data Size: {data_gb} GB ---")
    print(f"1. Unbounded (โตอิสระ): Exp={E_MAX_L5:.2f}x -> Need {mem_unbounded:.2f} GB [{status_unbounded}]")
    print(f"2. Bounded (Cardioid):  Exp={weighted_exp:.2f}x -> Need {mem_bounded:.2f} GB [{status_bounded}]")
    print(f"   (สมมติ: ข้อมูลสำคัญ {important_ratio*100}% ได้ไป L5, ที่เหลือถูกดักไว้ L0)")
    print(f"   Savings: {((mem_unbounded - mem_bounded)/mem_unbounded)*100:.1f}%\n")

# จำลองสถานการณ์จริง
# ข้อมูล 1.5 GB (ซึ่งถ้า Unbounded จะกิน ~11GB -> OOM แน่นอนบนเครื่อง 8GB)
simulate_bounded_growth(1.5, important_ratio=0.10) # Important 10%
simulate_bounded_growth(1.5, important_ratio=0.20) # Important 20%