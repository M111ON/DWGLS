# audio/ — Geo Audio + TTS/STT Section (parked Aug 12, 2026)

งาน audio ทั้งหมดพักไว้ที่ section นี้ หลัง merge จาก `feature/geo-audio-codec`
สภาพตอนพัก: **voice bridge ใช้ได้จริง** (pitch 96%), codec research อยู่ระหว่างทาง

## สถานะ

| Sub-system | ไฟล์หลัก | สถานะ |
|---|---|---|
| Voice Bridge (ASR→fingerprint→TTS→pitch) | `tools/voice_bridge.py` | ✅ production-ready |
| Geo audio codec (mel→20736) | `tools/geo_audio_v*.c` (v4-v21) | 🔬 research (decode 73.7%) |
| Whisper mel pipeline | `tools/whisper_mel_*.{c,py}` | ✅ ตรง whisper.cpp |
| Dictionary + projection | `tools/geo_dict_*.py`, `tools/geo_projection*.c` | 🔬 research |
| Word decoder | `tools/word_decoder.c`, `data/word_dictionary*.json` | 🔬 research |
| Data fixtures | `data/*.wav`, `data/whisper_mel_real.json` | ใช้เทียบ test |

## ใช้งาน

```bash
python audio/tools/voice_bridge.py --input <wav> --output <wav>       # voice bridge
python audio/tools/whisper_mel_extract.py <wav> <out.json>            # mel ตรง whisper.cpp
```

## หมายเหตุ

- Regenerable ขยะ ~110MB (npy/wav/mp3/log) จอดไว้ที่ `dropbag/audio-parked-2026-08-12/` — ลบได้ถ้าไม่ต้องการ
- Edge TTS ส่งออก MP3 แม้ชื่อ .wav → ต้อง ffmpeg convert (ดู skill dwgls-development)
- เสียงของจริง + ตารางพิสูจน์: `I:/Vaults/Research/` (vault)