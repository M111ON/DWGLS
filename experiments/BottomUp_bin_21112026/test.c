#include <stdio.h>
#include <stdint.h>

// 144x144 = 20,736 bits
// ใช้ uint32_t 648 ตัว (648 * 32 = 20,736)
uint32_t bitmap[648];

// ฟังก์ชันเปิดบิต (1) ที่ตำแหน่ง x, y
void set_bit(int x, int y) {
    int index = y * 144 + x;
    int word_idx = index / 32;
    int bit_idx = index % 32;
    bitmap[word_idx] |= (1U << bit_idx);
}

// ฟังก์ชันเช็คว่าบิตเปิดอยู่ไหม (1 = true, 0 = false)
int get_bit(int x, int y) {
    int index = y * 144 + x;
    int word_idx = index / 32;
    int bit_idx = index % 32;
    return (bitmap[word_idx] & (1U << bit_idx)) ? 1 : 0;
}

// ฟังก์ชัน Bottom-Up Cascade (แบบย่อ)
// ถ้าลูกเปิด ให้พ่อแม่ (ระดับบน) เปิดตาม
void cascade() {
    // สมมติว่าเราใช้ Quadtree ย่อขนาด 2x2 -> 1x1
    // ในที่นี้จะสาธิตแค่การยุบครึ่งนึง (72x72 -> 36x36) เพื่อให้เห็นภาพ
    for (int y = 0; y < 72; y++) {
        for (int x = 0; x < 72; x++) {
            int active = 0;
            // ถ้าในกลุ่ม 2x2 มีจุดไหนเปิด (1) ให้ถือว่า Active
            if (get_bit(x*2, y*2) || get_bit(x*2+1, y*2) || 
                get_bit(x*2, y*2+1) || get_bit(x*2+1, y*2+1)) {
                active = 1;
            }
            // ถ้า Active ให้เปิดบิตที่ตำแหน่ง (x+72, y) เพื่อแยกโซนแสดงผล (แค่ทดสอบ)
            if (active) {
                set_bit(x + 72, y);
            }
        }
    }
}

// ฟังก์ชันวาดภาพออกมาเป็นตัวหนังสือ (เพื่อให้ดูง่ายๆ ใน Terminal)
// ย่อส่วน 4x4 พิกเซล ให้เหลือ 1 ตัวอักษร
void print_ascii_map() {
    printf("\n--- BitMatrix 144x144 (ย่อ 4x4 = 1 char) ---\n");
    for (int y = 0; y < 144; y += 4) {
        for (int x = 0; x < 144; x += 4) {
            int count = 0;
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    if (get_bit(x + dx, y + dy)) count++;
                }
            }
            if (count > 0) printf("█"); // จุดขาว (Active)
            else printf(".");           // จุดดำ (Inactive)
        }
        printf("\n");
    }
    printf("--------------------------------------------\n");
}

int main() {
    // 1. ล้างค่าทั้งหมดให้เป็น 0 (สีดำ)
    for (int i = 0; i < 648; i++) bitmap[i] = 0;

    // 2. สุ่มจุด Active ขึ้นมา 3 จุด (สมมติว่าคือ Leaf)
    set_bit(10, 10);
    set_bit(50, 50);
    set_bit(120, 20);

    printf("Before Cascade:");
    print_ascii_map();

    // 3. รัน Bottom-Up Cascade
    cascade();

    printf("After Cascade (Pointers กางออก):");
    print_ascii_map();

    return 0;
}