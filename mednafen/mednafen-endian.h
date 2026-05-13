/******************************************************************************/
/* Mednafen - Multi-system Emulator                                           */
/******************************************************************************/
/* mednafen-endian.h:
**  Copyright (C) 2006-2017 Mednafen Team
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
*/

/*
 * Endian helpers for reading/writing little-endian integer values
 * to/from byte buffers.
 *
 * Host endianness is decided at preprocess time via MSB_FIRST.
 * Every helper below is split at the OUTER #ifdef: on a little-
 * endian host each helper is a bare native load/store; on a big-
 * endian host each helper folds a __builtin_bswap*() in. There is
 * no runtime branch, no MDFN_bswap* indirection, and nothing to
 * dead-strip - an LE TU never sees the BE source at all.
 *
 * Only the variants that have callers in this code base are kept.
 * Previously dead helpers removed (no callers anywhere in tree):
 *   - Big-endian readers/writers (de*msb / en*msb)
 *   - MDFN_de64lsb, MDFN_en64lsb, MDFN_de24msb, MDFN_bswap64
 *   - MDFN_densb_u32_aligned
 *   - MDFN_bswap16 / MDFN_bswap32  (folded into the BE bodies)
 *
 * The aligned variant (MDFN_de32lsb_aligned) uses
 * __builtin_assume_aligned to give the optimizer an alignment
 * guarantee on the cpu.c instruction-fetch hot path. On targets
 * where the builtin is unavailable (MSVC, ancient compilers) it
 * falls back to the unaligned form.
 *
 * Header is C89-compatible: no templates, no decltype, no
 * static_assert, no std::*. INLINE comes via mednafen-types.h.
 */

#ifndef __MDFN_ENDIAN_H
#define __MDFN_ENDIAN_H

#include <stddef.h>
#include <string.h>

/* mednafen-types.h provides the uint8/uint16/uint32/uint64 aliases
 * plus the INLINE macro used below. Pulling it in here lets a TU
 * include mednafen-endian.h without already having pulled in the
 * types header (state.c does this). */
#include "mednafen-types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bulk byte-buffer swap helpers. Used by savestate I/O (BE only) and
 * the CDDA raw-audio path (gated on a runtime flag, RawAudioMSBFirst,
 * that is orthogonal to host endianness), so the declarations are
 * unconditional. */
void Endian_A16_Swap(void *src, uint32 nelements);
void Endian_A32_Swap(void *src, uint32 nelements);
void Endian_A64_Swap(void *src, uint32 nelements);

#ifdef __cplusplus
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MDFN_BUILTIN_ASSUME_ALIGNED(p, n) __builtin_assume_aligned((p), (n))
#else
#define MDFN_BUILTIN_ASSUME_ALIGNED(p, n) (p)
#endif

/* 24-bit reader: byte-shift, endian-independent. The compiler folds
 * this to a 3-byte load + or on LE; on BE it stays three byte loads,
 * which is what we want because there's no 24-bit native type. */
static INLINE uint32 MDFN_de24lsb(const void *ptr)
{
   const uint8 *p = (const uint8 *)ptr;
   return ((uint32)p[0]) | ((uint32)p[1] << 8) | ((uint32)p[2] << 16);
}

#ifdef MSB_FIRST

/* ============================================================
 *  Big-endian host:  every LE read/write needs a byteswap.
 * ============================================================ */

#if defined(_MSC_VER)
#define MDFN_BSWAP16_(v) _byteswap_ushort(v)
#define MDFN_BSWAP32_(v) _byteswap_ulong(v)
#elif defined(__GNUC__) || defined(__clang__)
#define MDFN_BSWAP16_(v) __builtin_bswap16(v)
#define MDFN_BSWAP32_(v) __builtin_bswap32(v)
#else
#define MDFN_BSWAP16_(v) ((uint16)(((v) << 8) | ((v) >> 8)))
#define MDFN_BSWAP32_(v) ((uint32)(((v) << 24) | (((v) & 0xFF00U) << 8) \
                                | (((v) >> 8) & 0xFF00U) | ((v) >> 24)))
#endif

static INLINE uint16 MDFN_de16lsb(const void *ptr)
{
   uint16 v;
   memcpy(&v, ptr, sizeof(v));
   return MDFN_BSWAP16_(v);
}

static INLINE uint32 MDFN_de32lsb(const void *ptr)
{
   uint32 v;
   memcpy(&v, ptr, sizeof(v));
   return MDFN_BSWAP32_(v);
}

static INLINE uint32 MDFN_de32lsb_aligned(const void *ptr)
{
   uint32 v;
   memcpy(&v, MDFN_BUILTIN_ASSUME_ALIGNED(ptr, 4), sizeof(v));
   return MDFN_BSWAP32_(v);
}

static INLINE void MDFN_en16lsb(void *ptr, uint16 value)
{
   uint16 v = MDFN_BSWAP16_(value);
   memcpy(ptr, &v, sizeof(v));
}

static INLINE void MDFN_en32lsb(void *ptr, uint32 value)
{
   uint32 v = MDFN_BSWAP32_(value);
   memcpy(ptr, &v, sizeof(v));
}

#else  /* !MSB_FIRST */

/* ============================================================
 *  Little-endian host:  every LE read/write IS a native op.
 * ============================================================
 *  No swap, no branch, no helper-of-helper indirection. The
 *  memcpy of a constant 2/4 byte size folds to a single mov at
 *  -O1+, which is the best a native pointer deref could do
 *  without a strict-aliasing violation.
 */

static INLINE uint16 MDFN_de16lsb(const void *ptr)
{
   uint16 v;
   memcpy(&v, ptr, sizeof(v));
   return v;
}

static INLINE uint32 MDFN_de32lsb(const void *ptr)
{
   uint32 v;
   memcpy(&v, ptr, sizeof(v));
   return v;
}

static INLINE uint32 MDFN_de32lsb_aligned(const void *ptr)
{
   uint32 v;
   memcpy(&v, MDFN_BUILTIN_ASSUME_ALIGNED(ptr, 4), sizeof(v));
   return v;
}

static INLINE void MDFN_en16lsb(void *ptr, uint16 value)
{
   memcpy(ptr, &value, sizeof(value));
}

static INLINE void MDFN_en32lsb(void *ptr, uint32 value)
{
   memcpy(ptr, &value, sizeof(value));
}

#endif /* MSB_FIRST */

#endif
