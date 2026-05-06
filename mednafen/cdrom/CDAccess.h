#ifndef __MDFN_CDROMFILE_H
#define __MDFN_CDROMFILE_H

#include <stdint.h>
#include <boolean.h>

#include "CDUtility.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CDAccess: polymorphic disc-image reader.
 *
 * Previously a C++ abstract base class with four pure-virtual
 * methods.  Now a plain C struct with explicit function-pointer
 * vtable.  Each backend (Image / CCD / CHD / PBP) defines a
 * concrete struct that embeds `struct CDAccess base` as its first
 * member and a factory function `CDAccess_<X>_New` that allocates
 * the struct and installs the function pointers.
 *
 * Lifecycle: `cdaccess_open_image` dispatches on file extension
 * to the right factory.  Use `CDAccess_Read_Raw_Sector` etc. to
 * drive an instance, and `CDAccess_destroy` to tear it down.
 *
 * Backends MUST set Read_Raw_Sector, Read_TOC, Eject, and destroy.
 * Read_Raw_PW is optional - if NULL, CDAccess_Read_Raw_PW falls
 * back to a Read_Raw_Sector + memcpy of the 96 subchannel bytes.
 */
struct CDAccess
{
   bool (*Read_Raw_Sector)(struct CDAccess *self, uint8_t *buf, int32_t lba);
   bool (*Read_Raw_PW)    (struct CDAccess *self, uint8_t *buf, int32_t lba);
   bool (*Read_TOC)       (struct CDAccess *self, TOC *toc);
   void (*Eject)          (struct CDAccess *self, bool eject_status);
   void (*destroy)        (struct CDAccess *self);
};
typedef struct CDAccess CDAccess;

/* Factory: dispatches on path extension (.ccd, .pbp, .chd, else
 * Image).  On success returns a pointer and *success is true; on
 * failure may still return a non-NULL pointer (caller must
 * destroy it) and sets *success to false. */
CDAccess *cdaccess_open_image(bool *success, const char *path,
      bool image_memcache);

/* Public dispatch wrappers - invoke the vtable.  Identical to
 * `cda->Method(cda, ...)` but kept as named functions so call
 * sites and stack traces are readable. */
bool CDAccess_Read_Raw_Sector(CDAccess *cda, uint8_t *buf, int32_t lba);
bool CDAccess_Read_Raw_PW    (CDAccess *cda, uint8_t *buf, int32_t lba);
bool CDAccess_Read_TOC       (CDAccess *cda, TOC *toc);
void CDAccess_Eject          (CDAccess *cda, bool eject_status);
void CDAccess_destroy        (CDAccess *cda);

#ifdef __cplusplus
}
#endif

#endif
