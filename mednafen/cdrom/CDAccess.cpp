/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "../mednafen.h"

#include "CDAccess.h"
#include "CDAccess_Image.h"
#include "CDAccess_CCD.h"
#ifdef HAVE_PBP
#include "CDAccess_PBP.h"
#endif
#ifdef HAVE_CHD
#include "CDAccess_CHD.h"
#endif

extern "C" CDAccess *cdaccess_open_image(bool *success, const char *path,
      bool image_memcache)
{
   size_t    path_len = strlen(path);
   CDAccess *cda      = NULL;

   if (path_len >= 3 && !strcasecmp(path + path_len - 3, "ccd"))
      cda = CDAccess_CCD_New(success, path, image_memcache);
#ifdef HAVE_PBP
   else if (path_len >= 3 && !strcasecmp(path + path_len - 3, "pbp"))
      cda = CDAccess_PBP_New(success, path, image_memcache);
#endif
#ifdef HAVE_CHD
   else if (path_len >= 3 && !strcasecmp(path + path_len - 3, "chd"))
      cda = CDAccess_CHD_New(success, path, image_memcache);
#endif
   else
      cda = CDAccess_Image_New(success, path, image_memcache);

   /* Caller is responsible for destroying cda when *success is false. */
   return cda;
}

/* ------------------------------------------------------------------
 * Public dispatch wrappers - vtable invocations.
 *
 * Read_Raw_PW falls back to Read_Raw_Sector + 96-byte memcpy if a
 * backend doesn't supply a specialised implementation.  This is the
 * default that the old C++ CDAccess base class implemented; backends
 * that want the cheap subchannel-only path (CCD, CHD) override it.
 * ------------------------------------------------------------------ */

extern "C" bool CDAccess_Read_Raw_Sector(CDAccess *cda, uint8_t *buf,
      int32_t lba)
{
   return cda->Read_Raw_Sector(cda, buf, lba);
}

extern "C" bool CDAccess_Read_Raw_PW(CDAccess *cda, uint8_t *buf,
      int32_t lba)
{
   if (cda->Read_Raw_PW)
      return cda->Read_Raw_PW(cda, buf, lba);
   else
   {
      uint8_t tmpbuf[2352 + 96];
      if (!cda->Read_Raw_Sector(cda, tmpbuf, lba))
         return false;
      memcpy(buf, tmpbuf + 2352, 96);
      return true;
   }
}

extern "C" bool CDAccess_Read_TOC(CDAccess *cda, TOC *toc)
{
   return cda->Read_TOC(cda, toc);
}

extern "C" void CDAccess_Eject(CDAccess *cda, bool eject_status)
{
   cda->Eject(cda, eject_status);
}

extern "C" void CDAccess_destroy(CDAccess *cda)
{
   if (cda)
      cda->destroy(cda);
}
