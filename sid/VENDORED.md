# sid/ — vendored SID tensor-memory headers (DWGLS-owned copy)

## Why vendored
DWGLS cold-KV proofs (`tools/sid_kv_resume.c`, `tools/sid_kv_reanchor.c`,
`tools/sid_kv_compact.c`) need the SID store API. Instead of including
across repos (`-I I:/FGLS_new/...`), the exact headers we use live here,
so DWGLS builds self-contained and never touches the other team's tree.

## Origin (verified 2026-09-26)
FGLS_new @ `f19354f570fffe1701abbc65f7800884802c4fb6` (2026-08-12):

| file here              | source path                                              | state at vendor time |
|------------------------|----------------------------------------------------------|----------------------|
| `zone_card.h`          | `collection/src/zone_card.h`                             | tracked, unmodified  |
| `zone_card_sid.h`      | `collection/zone_card_sid.h`                             | tracked, unmodified  |
| `geo_jump.h`           | `collection/geo_jump_module/include/geo_jump.h`          | tracked, unmodified  |
| `tensor_memory.h`      | `collection/src/tensor_memory.h`                         | tracked, unmodified  |
| `adaptive_route_sid.h` | `collection/src/adaptive_route_sid.h`                    | untracked (DWGLS-origin adapter from Card #29, authored on our side) |

The three byte-identical files (`zone_card.h`, `zone_card_sid.h`,
`geo_jump.h`) match upstream byte-for-byte including CRLF endings.
The other two differ by exactly one line each: the flattened include
(`../zone_card_sid.h` → `zone_card_sid.h`, marked inline) because this
directory is flat while upstream nests `src/` one level down.

## Consumers
- `tools/sid_kv_resume.c`, `tools/sid_kv_reanchor.c`, `tools/sid_kv_compact.c`
  via `#include "adaptive_route_sid.h"` with `-I sid`.
- Makefile targets `sid-kv-resume-zc2`, `sid-kv-reanchor-zc2`, `sid-kv-compact-zc2`.

## Re-vendor rule
If upstream changes any of these files, copy the new version here and
update the table above (new hash + date). Never edit store semantics
here to work around a caller — fix the caller.
