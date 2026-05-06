#ifndef __MDFN_PSX_MASMEM_H
#define __MDFN_PSX_MASMEM_H

#include <stdlib.h>
#include <string.h>

#include <retro_inline.h>

#include "mednafen-types.h"

/*
 * Endian model: a single MSB_FIRST define determines host endianness
 * for the entire file. Defined => big-endian host (PowerPC consoles -
 * PS3, Xenon/Xbox 360, GameCube, Wii, ppc OSX); undefined => little-
 * endian host (everything else, which is the overwhelming majority of
 * libretro frontends today).
 *
 * The data the PS1 emulator stores in MainRAM / BIOSROM / ScratchRAM /
 * PIOMem is always laid out little-endian on disk and at runtime, so
 * the LE host case is a straight pointer dereference and the BE host
 * case is a byte-swap.
 */

#ifdef MSB_FIRST

/* Big-endian host: PS1 little-endian data needs to be byte-swapped on
 * every load and store. PowerPC has direct byte-reversed load/store
 * instructions; other (rare) BE hosts use a portable shift sequence. */

static INLINE uint16 LoadU16_LE(const uint16 *a)
{
#ifdef ARCH_POWERPC
   uint16 tmp;
   __asm__ ("lhbrx %0, %y1" : "=r"(tmp) : "Z"(*a));
   return tmp;
#else
   return (*a << 8) | (*a >> 8);
#endif
}

static INLINE uint32 LoadU32_LE(const uint32 *a)
{
#ifdef ARCH_POWERPC
   uint32 tmp;
   __asm__ ("lwbrx %0, %y1" : "=r"(tmp) : "Z"(*a));
   return tmp;
#else
   uint32 tmp = *a;
   return (tmp << 24) | ((tmp & 0xFF00) << 8) | ((tmp >> 8) & 0xFF00) | (tmp >> 24);
#endif
}

static INLINE void StoreU16_LE(uint16 *a, const uint16 v)
{
#ifdef ARCH_POWERPC
   __asm__ ("sthbrx %0, %y1" : : "r"(v), "Z"(*a));
#else
   *a = (v << 8) | (v >> 8);
#endif
}

static INLINE void StoreU32_LE(uint32 *a, const uint32 v)
{
#ifdef ARCH_POWERPC
   __asm__ ("stwbrx %0, %y1" : : "r"(v), "Z"(*a));
#else
   *a = (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
#endif
}

#else /* !MSB_FIRST */

/* Little-endian host: PS1 little-endian data is already in native
 * byte order. Every helper compiles to a single load/store. */

static INLINE uint16 LoadU16_LE(const uint16 *a)  { return *a; }
static INLINE uint32 LoadU32_LE(const uint32 *a)  { return *a; }
static INLINE void   StoreU16_LE(uint16 *a, const uint16 v) { *a = v; }
static INLINE void   StoreU32_LE(uint32 *a, const uint32 v) { *a = v; }

#endif /* MSB_FIRST */


/*
 * Mednafen's MultiAccessSizeMem is a fixed-size byte buffer that
 * supports aligned 8/16/24/32-bit access against the same backing
 * storage. Used for MainRAM, BIOSROM, ScratchRAM, and PIOMem.
 *
 * Originally a C++ template parameterised on size, max_unit_type,
 * and big_endian. Every instantiation passed (size, uint32, false),
 * so the last two parameters were dead weight, and the size was
 * only meaningful for the static buffer the union backed.
 *
 * The backing buffer is now heap-allocated (calloc - so it's
 * zero-initialised, matching the original value-initialised C++
 * union) and the struct holds a uint8_t pointer plus a size.
 * All four instances allocate their own buffer at construction.
 *
 * The struct is intentionally still C++; its instances live in
 * libretro.cpp and only that TU touches them via member calls.
 * The C-callable accessors in psx_mem.h forward to the same
 * member functions for cpu.c / dma.c consumers.
 */
#ifdef __cplusplus
struct MultiAccessSizeMem
{
   uint8_t  *data8;
   uint32_t  size;
   bool      owned;

   /* The original union exposed data16/data32 as aliases of the
    * same storage. data16 was unused; data32 is still touched in a
    * handful of places (the lightrec FastMap setup, OSD overlay
    * blits) so it stays as an alias-cast accessor. */
   uint32_t *get_data32(void) { return (uint32_t *)data8; }

   MultiAccessSizeMem() : data8(NULL), size(0), owned(false) {}

   /* Allocator: calloc gives us zero-initialised storage, matching
    * the C++ template's value-initialised in-class array. */
   bool init(uint32_t buf_size)
   {
      data8 = (uint8_t *)calloc(1, buf_size);
      size  = buf_size;
      owned = true;
      return data8 != NULL;
   }

   /* attach: take an existing buffer (typically lightrec mmap
    * region) without taking ownership.  The destructor must not
    * free attached buffers - they're owned by the caller. */
   void attach(uint8_t *buf, uint32_t buf_size)
   {
      data8 = buf;
      size  = buf_size;
      owned = false;
   }

   ~MultiAccessSizeMem()
   {
      if (owned)
         free(data8);
      data8 = NULL;
      size  = 0;
   }

   INLINE uint8_t ReadU8(uint32_t address)
   {
      return data8[address];
   }

   INLINE uint16_t ReadU16(uint32_t address)
   {
      return LoadU16_LE((uint16_t *)(data8 + address));
   }

   INLINE uint32_t ReadU32(uint32_t address)
   {
      return LoadU32_LE((uint32_t *)(data8 + address));
   }

   INLINE uint32_t ReadU24(uint32_t address)
   {
      return  ReadU8(address)
           | (ReadU8(address + 1) << 8)
           | (ReadU8(address + 2) << 16);
   }

   INLINE void WriteU8(uint32_t address, uint8_t value)
   {
      data8[address] = value;
   }

   INLINE void WriteU16(uint32_t address, uint16_t value)
   {
      StoreU16_LE((uint16_t *)(data8 + address), value);
   }

   INLINE void WriteU32(uint32_t address, uint32_t value)
   {
      StoreU32_LE((uint32_t *)(data8 + address), value);
   }

   INLINE void WriteU24(uint32_t address, uint32_t value)
   {
      WriteU8(address + 0, value >> 0);
      WriteU8(address + 1, value >> 8);
      WriteU8(address + 2, value >> 16);
   }

   template<typename T>
   INLINE T Read(uint32_t address)
   {
      if (sizeof(T) == 4)
         return ReadU32(address);
      if (sizeof(T) == 2)
         return ReadU16(address);
      return ReadU8(address);
   }

   template<typename T>
   INLINE void Write(uint32_t address, T value)
   {
      if (sizeof(T) == 4)
         WriteU32(address, value);
      else if (sizeof(T) == 2)
         WriteU16(address, value);
      else
         WriteU8(address, value);
   }
};
#endif /* __cplusplus */

#endif
