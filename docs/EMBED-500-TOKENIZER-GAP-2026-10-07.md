# Embed 500 `invalid unordered_map key` — e5-small Tokenizer Vocab Gap (2026-10-07)

## Summary (technical)

`llama-server --embedding` with `multilingual-e5-small-q8_0-fixed.gguf` returns HTTP 500
`invalid unordered_map<K, T> key` for inputs containing any of 63 Unicode chars absent from
the model tokenizer vocab. Single-char repro: `₂` (U+2082) alone → 500 deterministic,
while `café 中文` → OK. Root cause is a tokenizer vocab gap, not an encoding bug.

Impact: `build/reindex_docs.py` embeds each 400-char chunk with 4-attempt retry + backoff,
so each bad chunk cost ~10s then skipped (`fails` counter). Docs with Thai text
(e.g. `ACTION-PLAN-2026-08-22.md` chunks 0-5) failed wholesale on the unpatched run.

Fix: `sanitize()` in `build/reindex_docs.py` — `unicodedata.normalize("NFKC", text)` plus
an ascii map for the 12 residue chars NFKC cannot fix. Verified live: 63/63 previously
failing chars now embed OK. Patched reindex: 121 docs, 2881 entries, dims {384},
`fails=0`, `bad_json=0`.

## สรุปบ้านๆ

โปรแกรมแปลงข้อความเป็นตัวเลขไม่รู้จักอักษร 63 ตัว (ตัวยก ตัวห้อย สระำ เครื่องหมายคณิต
อิโมจิ ตัวเลขในวงกลม) เจอตัวใดตัวหนึ่งคือล่มทั้งชิ้น วิธีแก้คือแปลอักษรพวกนั้นเป็นตัว
ธรรมดาก่อนส่ง (₂→2, ⏭→"next ") ครบทั้ง 63 ตัว รอบที่แก้แล้ววิ่งจบไม่พลาดสักชิ้น

## Timeline

| Time | Event |
|------|-------|
| 23:4x | Serve rebuilt (139301B, md5 `6eca3007`), booted PID 11244, search 302/302 OK |
| 23:5x | Full reindex (PID 5480) stalls at 211→241 lines; embed log shows repeating 500s |
| 23:55 | Bisect: `₂` alone → 500. NFKC patch applied to `reindex_docs.py` |
| 00:0x | Sweep: 281 distinct non-ascii chars in `docs/*.md` → 63 fail; NFKC fixes 51, 12 mapped by hand |
| 00:03 | Old run stuck at 455 lines (CPU frozen, stuck chunk has no known-bad chars — unknown hang). Killed, partial backed up to `/tmp/kv_index.455.partial.bak` |
| 00:04 | Patched reindex (PID 9948): 455 → 1268 (45s) → 2881, exit 0, `fails=0` |
| 00:06 | Serve restarted (PID 7444) on full index: 2881/2881 |
| 00:07 | `POST /v1/state/regrow` → `{nent:2881, K:8}`; search mode **anchor**, buckets [2,7,4], scored 1650/2881 |
| 00:0x | Smoke 15/15 PASS. Handoff items 1-4 complete |

## The 63 bad chars (by category)

- Sub/superscripts: ² ³ ¹ ⁰ ⁴ ⁵ ⁶ ⁷ ⁸ ⁺ ⁻ ⁿ ₀ ₁ ₂ ₃ ₄ ₅ ₆ ₙ ¼ µ ᵃ ᵇ ᵏ ᵗ ᵢ
- Thai: ำ (U+0E33 SARA AM — NFKC folds to combining pair U+0E4D+U+0E32, embeds OK)
- Fractions/symbols: ⅓ ⅔ … ″ ℝ ℤ
- Math (NFKC-proof, ascii-mapped): ∉ ∋ ∖ ∝ ∤ ≅ ⋀ ⋁ ⟺
- Emoji/shapes (NFKC-proof, ascii-mapped): ⏭ ⏸ ⬠
- Circled digits: ①–⑰ (NFKC folds to plain digits, embeds OK)

Residue map (12 entries, verified live):

```
∉→" not in "  ∋→" contains "  ∖→" minus "  ∝→" proportional to "
∤→" does not divide "  ≅→" ~= "  ⋀→" AND "  ⋁→" OR "
⏭→" next "  ⏸→" pause "  ⟺→" <=> "  ⬠→" pentagon "
```

## Fix location

`build/reindex_docs.py` — `sanitize()` called at top of `embed()`.
NOTE: `build/` is gitignored, so this fix lives outside git. The `tools/gguf_lazy_serve.c`
parser fix (`lz_json_val` tolerant spacing) is a separate uncommitted change — see
`docs/HANDOFF-ANN-2026-10-07.md`.

## Verification

- Single-char sweep: 63/63 previously-failing chars embed OK after `sanitize()`
- `py_compile build/reindex_docs.py` OK
- Reindex exit 0: `docs: 121, dim=384, done fails=0`
- Index: 2881 entries, dims {384}, 121 files, bad_json 0
- Search `tesspack format spec` → 2881/2881, PIPELINE-MAP 0.8793
- Search `kv remap hybrid rail` → mode anchor, buckets [2,7,4], 1650/2881 scored
- `make test-smoke` PASS 15/15 FAIL 0

## Repro (single char)

```bash
curl -s -X POST http://127.0.0.1:18095/embedding \
  -H "Content-Type: application/json" -d '{"content":"₂"}'
# -> 500 invalid unordered_map<K, T> key  (before sanitize)
```

## Remaining

1. Commit `tools/gguf_lazy_serve.c` parser fix (uncommitted)
2. Full `make test` (TIER1+TIER2) at session end
3. Decide where `sanitize()` should live permanently (`build/` is gitignored —
   either track the script or move the function into a tracked helper)
4. Unknown hang at GOE-CLIMATE-FILTER chunk 11 (no known-bad chars, CPU frozen) —
   not reproduced on direct embed; left as open question, not blocking

## Gotchas hit this session

- `llama-server` v040 dir binary is a 9KB silent stub (exit 0 even on `--help`) — use b10830
- `lz_entry_parse` no-space `"sid":"` vs `json.dumps` spaced `"sid": "` dropped all 302 entries
- `.exe` locked while running — rebuild to `_new.exe` then move
- Pipe through `head` masks gcc exit code — check binary size/md5 directly
- `make` missing — use `/c/mingw64/bin/mingw32-make.exe`
- `taskkill` needs `MSYS2_ARG_CONV_EXCL="*"` prefix
- python is `I:/python3.14.4/python.exe`, not `python3`
