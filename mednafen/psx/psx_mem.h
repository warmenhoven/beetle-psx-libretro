#ifndef __MDFN_PSX_PSX_MEM_H
#define __MDFN_PSX_PSX_MEM_H

#include <stdint.h>

/*
 * C-linkage accessors for MainRAM and ScratchRAM.  The underlying
 * storage is MultiAccessSizeMem<N> singletons declared in
 * libretro.cpp, and the C++ Read/Write member functions on
 * MultiAccessSizeMem are the public access path.  C TUs (dma.c,
 * cpu.c, ...) can't include masmem.h because the type is a class
 * template, so this header exposes the accesses they need as plain
 * C functions.
 *
 * Implementations are in libretro.cpp at file scope where each
 * memory is defined, with `extern "C"` linkage; they're one-line
 * forwarders to the existing member functions and inline away under
 * -O2 / LTO.
 *
 * Address parameters are pre-masked by the caller (typically with
 * `& 0x1FFFFC` / `& 0x3FF`) - same convention as the C++ ReadU32 /
 * WriteU32 call sites.
 */

#ifdef __cplusplus
extern "C" {
#endif

uint32_t MainRAM_ReadU32(uint32_t address);
void     MainRAM_WriteU32(uint32_t address, uint32_t value);

uint8_t  ScratchRAM_ReadU8 (uint32_t address);
uint16_t ScratchRAM_ReadU16(uint32_t address);
uint32_t ScratchRAM_ReadU24(uint32_t address);
uint32_t ScratchRAM_ReadU32(uint32_t address);
void     ScratchRAM_WriteU8 (uint32_t address, uint8_t  value);
void     ScratchRAM_WriteU16(uint32_t address, uint16_t value);
void     ScratchRAM_WriteU24(uint32_t address, uint32_t value);
void     ScratchRAM_WriteU32(uint32_t address, uint32_t value);

/* ScratchRAM_data8 returns &ScratchRAM->data8[0].  Used by the
 * lightrec map setup and the savestate SFARRAYN that names the
 * region "ScratchRAM.data8" - both want the raw byte pointer. */
uint8_t *ScratchRAM_data8(void);

/* MainRAM and BIOSROM byte-pointer accessors.  Used by the lightrec
 * init path to publish the raw buffer addresses to the dynarec, and
 * by mempatcher's IsC isolate-cache shadow memcpy. */
uint8_t *MainRAM_data8(void);
uint8_t *BIOSROM_data8(void);

#ifdef __cplusplus
}
#endif

#endif
