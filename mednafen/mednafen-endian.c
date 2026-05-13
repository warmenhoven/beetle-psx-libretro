/******************************************************************************/
/* Mednafen - Multi-system Emulator                                           */
/******************************************************************************/
/* mednafen-endian.cpp:
**  Copyright (C) 2006-2016 Mednafen Team
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "mednafen-types.h"
#include "mednafen-endian.h"

/*
 * Bulk byte-swap helpers. Each operates in place on `nelements`
 * aligned values. The functions are only exercised when MSB_FIRST is
 * defined (savestate I/O and CDDA decode); the LE build never calls
 * them but they're still emitted in the TU.
 *
 * Implementation uses memcpy-bswap-memcpy rather than a byte-by-byte
 * scalar swap so modern compilers can fold the body into a single
 * load - bswap - store sequence and (for the 32/64 cases) vectorize
 * the loop. The memcpys silence strict-aliasing concerns and become
 * mov / movbe instructions at -O2.
 */

#if defined(__GNUC__) || defined(__clang__)
#define MDFN_BSWAP16(v) __builtin_bswap16(v)
#define MDFN_BSWAP32(v) __builtin_bswap32(v)
#define MDFN_BSWAP64(v) __builtin_bswap64(v)
#elif defined(_MSC_VER)
#include <stdlib.h>
#define MDFN_BSWAP16(v) _byteswap_ushort(v)
#define MDFN_BSWAP32(v) _byteswap_ulong(v)
#define MDFN_BSWAP64(v) _byteswap_uint64(v)
#else
static INLINE uint16 MDFN_BSWAP16(uint16 v)
{
   return (uint16)((v << 8) | (v >> 8));
}
static INLINE uint32 MDFN_BSWAP32(uint32 v)
{
   return  (v << 24)
        | ((v & 0xFF00U) << 8)
        | ((v >> 8) & 0xFF00U)
        |  (v >> 24);
}
static INLINE uint64 MDFN_BSWAP64(uint64 v)
{
   return  (v << 56)
        | ((v & 0xFF00ULL)             << 40)
        | ((v & 0xFF0000ULL)           << 24)
        | ((v & 0xFF000000ULL)         <<  8)
        | ((v >>  8) & 0xFF000000ULL)
        | ((v >> 24) & 0xFF0000ULL)
        | ((v >> 40) & 0xFF00ULL)
        |  (v >> 56);
}
#endif

void Endian_A16_Swap(void *src, uint32 nelements)
{
   uint32 i;
   uint8 *nsrc = (uint8 *)src;

   for (i = 0; i < nelements; i++)
   {
      uint16 v;
      memcpy(&v, nsrc + i * 2, sizeof(v));
      v = MDFN_BSWAP16(v);
      memcpy(nsrc + i * 2, &v, sizeof(v));
   }
}

void Endian_A32_Swap(void *src, uint32 nelements)
{
   uint32 i;
   uint8 *nsrc = (uint8 *)src;

   for (i = 0; i < nelements; i++)
   {
      uint32 v;
      memcpy(&v, nsrc + i * 4, sizeof(v));
      v = MDFN_BSWAP32(v);
      memcpy(nsrc + i * 4, &v, sizeof(v));
   }
}

void Endian_A64_Swap(void *src, uint32 nelements)
{
   uint32 i;
   uint8 *nsrc = (uint8 *)src;

   for (i = 0; i < nelements; i++)
   {
      uint64 v;
      memcpy(&v, nsrc + i * 8, sizeof(v));
      v = MDFN_BSWAP64(v);
      memcpy(nsrc + i * 8, &v, sizeof(v));
   }
}
