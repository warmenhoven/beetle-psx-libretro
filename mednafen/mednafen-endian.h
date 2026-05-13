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
 * Bulk byte-buffer swap helpers, used by savestate I/O (BE only) and
 * the CDDA raw-audio path (gated on a runtime RawAudioMSBFirst flag
 * that's orthogonal to host endianness).
 *
 * The per-value LE decode / encode helpers (MDFN_de16lsb,
 * MDFN_de32lsb, MDFN_de24lsb, MDFN_de32lsb_aligned, MDFN_en16lsb,
 * MDFN_en32lsb) have been removed. They were inlined at every call
 * site, with an explicit #ifdef MSB_FIRST picking between a native
 * memcpy (LE host) and a byte-shift OR/assemble (BE host).
 */

#ifndef __MDFN_ENDIAN_H
#define __MDFN_ENDIAN_H

#include "mednafen-types.h"

#ifdef __cplusplus
extern "C" {
#endif

void Endian_A16_Swap(void *src, uint32 nelements);
void Endian_A32_Swap(void *src, uint32 nelements);
void Endian_A64_Swap(void *src, uint32 nelements);

#ifdef __cplusplus
}
#endif

#endif
