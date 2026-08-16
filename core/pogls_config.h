/*
 * pogls_config.h
 * ─────────────────────────────────────────────────────────────
 * Portable configuration layer — replaces all hardcoded paths/constants
 *
 * Override at compile time:
 *   gcc -DPOGLS_SESSION_NONCE=0xYOURVALUE ...
 *   gcc -DPOGLS_BOND_VERIFY_BITS=32 ...
 *
 * Or define before #include "pogls_config.h"
 * ─────────────────────────────────────────────────────────────
 */

#ifndef POGLS_CONFIG_H
#define POGLS_CONFIG_H

#include <stdint.h>

/* ── BOND VERIFICATION STRENGTH ─────────────────────────────
 * POGLS_BOND_VERIFY_BITS = how many bits of combined hash must match
 *
 * Old: 16-bit top mask == 0x9009   → 1/65536 false positive (~0.0015%)
 * New: 32-bit mask (default)       → 1/4B    false positive (~2.3e-8%)
 *      48-bit option               → 1/281T  (overkill for most uses)
 *
 * Recommend: 32 for API/SaaS, 48 for Ghost Storage (provable deletion)
 */
#ifndef POGLS_BOND_VERIFY_BITS
#  define POGLS_BOND_VERIFY_BITS  32
#endif

#if POGLS_BOND_VERIFY_BITS == 16
#  define POGLS_BOND_VERIFY_MASK    UINT64_C(0xFFFF000000000000)
#  define POGLS_BOND_VERIFY_TARGET  UINT64_C(0x9009000000000000)
#elif POGLS_BOND_VERIFY_BITS == 32
#  define POGLS_BOND_VERIFY_MASK    UINT64_C(0xFFFFFFFF00000000)
#  define POGLS_BOND_VERIFY_TARGET  UINT64_C(0x9009900900000000)
#elif POGLS_BOND_VERIFY_BITS == 48
#  define POGLS_BOND_VERIFY_MASK    UINT64_C(0xFFFFFFFFFFFF0000)
#  define POGLS_BOND_VERIFY_TARGET  UINT64_C(0x9009900990090000)
#else
#  error "POGLS_BOND_VERIFY_BITS must be 16, 32, or 48"
#endif

/* ── SESSION NONCE ───────────────────────────────────────────
 * Injected into bond_verify mixing pass.
 * Prevents replay: bond valid in session A is invalid in session B.
 *
 * Set per-deployment. Override at compile time or runtime via
 * pogls_config_set_nonce().
 *
 * Default = 0 (backward-compat with existing pieces, no replay protection)
 * Production = non-zero unique value per deployment/session
 */
#ifndef POGLS_SESSION_NONCE
#  define POGLS_SESSION_NONCE  UINT64_C(0x0000000000000000)
#endif

/* Runtime nonce override — call once at startup */
static uint64_t _pogls_runtime_nonce = POGLS_SESSION_NONCE;

static inline void pogls_config_set_nonce(uint64_t nonce) {
    _pogls_runtime_nonce = nonce;
}

static inline uint64_t pogls_config_get_nonce(void) {
    return _pogls_runtime_nonce;
}

/* ── STORAGE PATHS ───────────────────────────────────────────
 * All paths relative or env-driven. No hardcoded absolute paths.
 *
 * Priority: env var > compile-time define > default
 */
#include <stdlib.h>
#include <string.h>

static inline const char *pogls_config_root(void) {
    const char *env = getenv("POGLS_ROOT");
    if (env && *env) return env;
#ifdef POGLS_ROOT_PATH
    return POGLS_ROOT_PATH;
#else
    return ".";   /* cwd fallback — always works */
#endif
}

static inline const char *pogls_config_blob_dir(void) {
    const char *env = getenv("POGLS_BLOB_DIR");
    if (env && *env) return env;
    return "blobs";   /* relative to pogls_config_root() */
}

static inline const char *pogls_config_wallet_dir(void) {
    const char *env = getenv("POGLS_WALLET_DIR");
    if (env && *env) return env;
    return "wallets";
}

/* ── PLATFORM DETECTION ──────────────────────────────────────*/
#if defined(_WIN32) || defined(_WIN64)
#  define POGLS_PLATFORM_WINDOWS 1
#  define POGLS_PATH_SEP         '\\'
#else
#  define POGLS_PLATFORM_UNIX    1
#  define POGLS_PATH_SEP         '/'
#endif

/* ── VERSION ─────────────────────────────────────────────────*/
#define POGLS_BOND_VERSION_MAJOR  1
#define POGLS_BOND_VERSION_MINOR  1
#define POGLS_BOND_VERSION_PATCH  0
#define POGLS_BOND_VERSION_STR    "1.1.0"

#endif /* POGLS_CONFIG_H */
