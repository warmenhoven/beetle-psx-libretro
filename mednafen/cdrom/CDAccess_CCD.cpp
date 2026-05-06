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

#include <new>
#include <stdio.h>
#include <compat/msvc.h>

#include <limits>
#include <limits.h>
#include <map>
#include <string>

#include "../mednafen.h"
#include "../error.h"
#include "../general.h"
#include "../FileStream.h"
#include "../MemoryStream.h"
#include "CDAccess.h"
#include "misc.h"
#include "CDAccess_CCD.h"
#include "CDUtility.h"

typedef std::map<std::string, std::string> CCD_Section;

/* ------------------------------------------------------------------
 * Concrete struct - embeds CDAccess as the first member.  Stays a
 * .cpp because CCD's parser still uses std::map and std::string;
 * those will move to plain C in a follow-up stage.
 * ------------------------------------------------------------------ */

struct CDAccess_CCD
{
   CDAccess  base;

   Stream   *img_stream;
   Stream   *sub_stream;
   size_t    img_numsectors;
   TOC       tocd;
};

/* CCD_ReadInt: parse an integer from a CCD section.
 *
 * Sets *ok to false on error (missing property, malformed value).
 * Callers MUST initialize *ok to true before a sequence of calls and
 * check it after the sequence; once *ok is false, subsequent calls
 * are still safe (they short-circuit return 0) so error propagation
 * can be batched at meaningful boundaries rather than after every line. */
template<typename T>
static T CCD_ReadInt(bool *ok, CCD_Section &s, const std::string &propname,
      const bool have_defval = false, const int defval = 0)
{
   const char *vp;
   char *ep           = NULL;
   int scan_base      = 10;
   size_t scan_offset = 0;
   long ret           = 0;
   CCD_Section::iterator zit;

   if (!*ok)
      return 0;

   zit = s.find(propname);

   if (zit == s.end())
   {
      if (have_defval)
         return defval;
      MDFN_Error(0, "Missing property: %s", propname.c_str());
      *ok = false;
      return 0;
   }

   const std::string &v = zit->second;

   if (v.length() >= 3 && v[0] == '0' && v[1] == 'x')
   {
      scan_base   = 16;
      scan_offset = 2;
   }

   vp = v.c_str() + scan_offset;

   if (std::numeric_limits<T>::is_signed)
      ret = strtol(vp, &ep, scan_base);
   else
      ret = strtoul(vp, &ep, scan_base);

   if (!vp[0] || ep[0])
   {
      MDFN_Error(0, "Property %s: Malformed integer: %s",
            propname.c_str(), v.c_str());
      *ok = false;
      return 0;
   }

   return ret;
}

/* ------------------------------------------------------------------
 * Body methods.
 * ------------------------------------------------------------------ */

static bool CDAccess_CCD_CheckSubQSanity(CDAccess_CCD *self)
{
   /* Checks for Q subchannel mode 1 (current time) data that has a
    * correct checksum but is nonsensical or corrupted; this is the
    * case for some bad rips floating around on the Internet.
    * Allowing them through would cause emulation problems. */
   size_t   s;
   size_t   checksum_pass_counter = 0;
   int      prev_lba   = INT_MAX;
   uint8_t  prev_track = 0;

   (void)prev_lba;

   for (s = 0; s < self->img_numsectors; s++)
   {
      uint8_t adr;
      union
      {
         uint8_t full[96];
         struct
         {
            uint8_t pbuf[12];
            uint8_t qbuf[12];
         };
      } buf;

      stream_seek(self->sub_stream, s * 96, SEEK_SET);
      stream_read(self->sub_stream, buf.full, 96);

      if (!subq_check_checksum(buf.qbuf))
         continue;

      adr = buf.qbuf[0] & 0xF;

      if (adr == 0x01)
      {
         int     lba;
         uint8_t track;
         uint8_t track_bcd = buf.qbuf[1];
         uint8_t index_bcd = buf.qbuf[2];
         uint8_t rm_bcd    = buf.qbuf[3];
         uint8_t rs_bcd    = buf.qbuf[4];
         uint8_t rf_bcd    = buf.qbuf[5];
         uint8_t am_bcd    = buf.qbuf[7];
         uint8_t as_bcd    = buf.qbuf[8];
         uint8_t af_bcd    = buf.qbuf[9];

         if (!BCD_is_valid(track_bcd) || !BCD_is_valid(index_bcd)
               || !BCD_is_valid(rm_bcd) || !BCD_is_valid(rs_bcd)
               || !BCD_is_valid(rf_bcd) || !BCD_is_valid(am_bcd)
               || !BCD_is_valid(as_bcd) || !BCD_is_valid(af_bcd)
               || rs_bcd > 0x59 || rf_bcd > 0x74
               || as_bcd > 0x59 || af_bcd > 0x74)
         {
            MDFN_Error(0,
                  "Garbage subchannel Q data detected (bad BCD/out of range): %02x:%02x:%02x %02x:%02x:%02x",
                  rm_bcd, rs_bcd, rf_bcd, am_bcd, as_bcd, af_bcd);
            return false;
         }

         lba   = ((BCD_to_U8(am_bcd) * 60 + BCD_to_U8(as_bcd)) * 75
                  + BCD_to_U8(af_bcd)) - 150;
         track = BCD_to_U8(track_bcd);

         prev_lba = lba;

         if (track < prev_track)
         {
            MDFN_Error(0, "Garbage subchannel Q data detected (bad track number)");
            return false;
         }

         prev_track = track;
         checksum_pass_counter++;
      }
   }

   return true;
}

static bool CDAccess_CCD_Load(CDAccess_CCD *self, const char *path,
      bool image_memcache)
{
   /* All return paths past mdfn_filestream_init pass through cleanup:
    * which closes cf.  cf is in-place so use stream_close (NOT destroy). */
   FileStream  cf;
   bool        ok = true;
   std::map<std::string, CCD_Section> Sections;
   std::string linebuf;
   std::string cur_section_name;
   std::string dir_path, file_base, file_ext;
   char        img_extsd[4] = { 'i', 'm', 'g', 0 };
   char        sub_extsd[4] = { 's', 'u', 'b', 0 };

   mdfn_filestream_init(&cf, path);
   if (!mdfn_filestream_is_open(&cf))
   {
      MDFN_Error(0, "CCD: failed to open \"%s\"", path);
      ok = false;
      goto cleanup;
   }

   MDFN_GetFilePathComponents(path, &dir_path, &file_base, &file_ext);

   if (file_ext.length() == 4 && file_ext[0] == '.')
   {
      int          i;
      signed char  av        = -1;
      signed char  extupt[3] = { -1, -1, -1 };

      for (i = 1; i < 4; i++)
      {
         if (file_ext[i] >= 'A' && file_ext[i] <= 'Z')
            extupt[i - 1] = 'A' - 'a';
         else if (file_ext[i] >= 'a' && file_ext[i] <= 'z')
            extupt[i - 1] = 0;
      }

      for (i = 0; i < 3; i++)
      {
         if (extupt[i] != -1)
            av = extupt[i];
         else
            extupt[i] = av;
      }

      if (av == -1)
         av = 0;

      for (i = 0; i < 3; i++)
      {
         if (extupt[i] == -1)
            extupt[i] = av;
      }

      for (i = 0; i < 3; i++)
      {
         img_extsd[i] += extupt[i];
         sub_extsd[i] += extupt[i];
      }
   }

   linebuf.reserve(256);

   while (stream_get_line_string(&cf.base, linebuf) >= 0)
   {
      MDFN_trim(linebuf);

      if (linebuf.length() == 0)
         continue;

      if (linebuf[0] == '[')
      {
         if (linebuf.length() < 3 || linebuf[linebuf.length() - 1] != ']')
         {
            MDFN_Error(0, "Malformed section specifier: %s",
                  linebuf.c_str());
            ok = false;
            goto cleanup;
         }

         cur_section_name = linebuf.substr(1, linebuf.length() - 2);
         MDFN_strtoupper(cur_section_name);
      }
      else
      {
         std::string  k, v;
         const size_t feqpos = linebuf.find('=');
         const size_t leqpos = linebuf.rfind('=');

         if (feqpos == std::string::npos || feqpos != leqpos)
         {
            MDFN_Error(0, "Malformed value pair specifier: %s",
                  linebuf.c_str());
            ok = false;
            goto cleanup;
         }

         k = linebuf.substr(0, feqpos);
         v = linebuf.substr(feqpos + 1);

         MDFN_trim(k);
         MDFN_trim(v);

         MDFN_strtoupper(k);

         Sections[cur_section_name][k] = v;
      }
   }

   {
      unsigned     te;
      bool         inner_ok          = true;
      CCD_Section &ds                = Sections["DISC"];
      unsigned     toc_entries       = CCD_ReadInt<unsigned>(&inner_ok, ds, "TOCENTRIES");
      unsigned     num_sessions      = CCD_ReadInt<unsigned>(&inner_ok, ds, "SESSIONS");
      bool         data_tracks_scrambled =
            CCD_ReadInt<unsigned>(&inner_ok, ds, "DATATRACKSSCRAMBLED") != 0;

      if (!inner_ok)
      {
         ok = false;
         goto cleanup;
      }

      if (num_sessions != 1)
      {
         MDFN_Error(0, "Unsupported number of sessions: %u", num_sessions);
         ok = false;
         goto cleanup;
      }

      if (data_tracks_scrambled)
      {
         MDFN_Error(0, "Scrambled CCD data tracks currently not supported.");
         ok = false;
         goto cleanup;
      }

      for (te = 0; te < toc_entries; te++)
      {
         char     tmpbuf[64];
         uint8_t  point;
         uint8_t  adr;
         uint8_t  control;
         uint8_t  pmin;
         uint8_t  psec;
         uint8_t  pframe;
         signed   plba;
         unsigned session;

         snprintf(tmpbuf, sizeof(tmpbuf), "ENTRY %u", te);
         {
            CCD_Section &ts = Sections[std::string(tmpbuf)];
            session = CCD_ReadInt<unsigned>(&inner_ok, ts, "SESSION");
            point   = CCD_ReadInt<uint8_t>(&inner_ok, ts, "POINT");
            adr     = CCD_ReadInt<uint8_t>(&inner_ok, ts, "ADR");
            control = CCD_ReadInt<uint8_t>(&inner_ok, ts, "CONTROL");
            pmin    = CCD_ReadInt<uint8_t>(&inner_ok, ts, "PMIN");
            psec    = CCD_ReadInt<uint8_t>(&inner_ok, ts, "PSEC");
            pframe  = CCD_ReadInt<uint8_t>(&inner_ok, ts, "PFRAME");
            plba    = CCD_ReadInt<signed>(&inner_ok, ts, "PLBA");
            (void)pframe;
         }

         if (!inner_ok)
         {
            ok = false;
            goto cleanup;
         }

         if (session != 1)
         {
            MDFN_Error(0, "Unsupported TOC entry Session value: %u", session);
            ok = false;
            goto cleanup;
         }

         /* Reference: ECMA-394, page 5-14 */
         if (point >= 1 && point <= 99)
         {
            self->tocd.tracks[point].adr     = adr;
            self->tocd.tracks[point].control = control;
            self->tocd.tracks[point].lba     = plba;
         }
         else
         {
            switch (point)
            {
               default:
                  MDFN_Error(0, "Unsupported TOC entry Point value: %u", point);
                  ok = false;
                  goto cleanup;
               case 0xA0:
                  self->tocd.first_track = pmin;
                  self->tocd.disc_type   = psec;
                  break;
               case 0xA1:
                  self->tocd.last_track  = pmin;
                  break;
               case 0xA2:
                  self->tocd.tracks[100].adr     = adr;
                  self->tocd.tracks[100].control = control;
                  self->tocd.tracks[100].lba     = plba;
                  break;
            }
         }
      }
   }

   /* Convenience leadout track duplication. */
   if (self->tocd.last_track < 99)
      self->tocd.tracks[self->tocd.last_track + 1] = self->tocd.tracks[100];

   /* Open image stream. */
   {
      std::string image_path = MDFN_EvalFIP(dir_path,
            file_base + std::string(".") + std::string(img_extsd));
      FileStream *str = mdfn_filestream_new(image_path.c_str());
      int64_t     ss;

      if (!mdfn_filestream_is_open(str))
      {
         MDFN_Error(0, "Could not open CCD image \"%s\"",
               image_path.c_str());
         if (str)
            stream_destroy(&str->base);
         ok = false;
         goto cleanup;
      }

      if (image_memcache)
      {
         /* mdfn_memstream_new_from_stream consumes &str->base regardless. */
         struct MemoryStream *mem = mdfn_memstream_new_from_stream(&str->base);
         if (!mdfn_memstream_is_valid(mem))
         {
            MDFN_Error(0,
                  "Could not load CCD image \"%s\" into memory",
                  image_path.c_str());
            if (mem)
               stream_destroy(&mem->base);
            ok = false;
            goto cleanup;
         }
         self->img_stream = &mem->base;
      }
      else
         self->img_stream = &str->base;

      ss = stream_size(self->img_stream);

      if (ss % 2352)
      {
         MDFN_Error(0, "CCD image size is not evenly divisible by 2352.");
         ok = false;
         goto cleanup;
      }

      self->img_numsectors = ss / 2352;
   }

   {
      /* Open subchannel stream. */
      std::string sub_path = MDFN_EvalFIP(dir_path,
            file_base + std::string(".") + std::string(sub_extsd));
      FileStream *str = mdfn_filestream_new(sub_path.c_str());

      if (!mdfn_filestream_is_open(str))
      {
         MDFN_Error(0, "Could not open CCD subchannel \"%s\"",
               sub_path.c_str());
         if (str)
            stream_destroy(&str->base);
         ok = false;
         goto cleanup;
      }

      if (image_memcache)
      {
         struct MemoryStream *mem = mdfn_memstream_new_from_stream(&str->base);
         if (!mdfn_memstream_is_valid(mem))
         {
            MDFN_Error(0,
                  "Could not load CCD subchannel \"%s\" into memory",
                  sub_path.c_str());
            if (mem)
               stream_destroy(&mem->base);
            ok = false;
            goto cleanup;
         }
         self->sub_stream = &mem->base;
      }
      else
         self->sub_stream = &str->base;

      if (stream_size(self->sub_stream)
            != (uint64_t)self->img_numsectors * 96)
      {
         MDFN_Error(0, "CCD SUB file size mismatch.");
         ok = false;
         goto cleanup;
      }
   }

   if (!CDAccess_CCD_CheckSubQSanity(self))
   {
      ok = false;
      goto cleanup;
   }

cleanup:
   stream_close(&cf.base);
   return ok;
}

static void CDAccess_CCD_Cleanup(CDAccess_CCD *self)
{
   if (self->img_stream)
   {
      stream_destroy(self->img_stream);
      self->img_stream = NULL;
   }
   if (self->sub_stream)
   {
      stream_destroy(self->sub_stream);
      self->sub_stream = NULL;
   }
}

static bool CDAccess_CCD_Read_Raw_Sector(CDAccess *base_self, uint8_t *buf,
      int32_t lba)
{
   CDAccess_CCD *self = (CDAccess_CCD *)base_self;
   uint8_t       sub_buf[96];

   if (lba < 0 || (size_t)lba >= self->img_numsectors)
   {
      MDFN_Error(0, "LBA out of range.");
      return false;
   }

   stream_seek(self->img_stream, lba * 2352, SEEK_SET);
   stream_read(self->img_stream, buf, 2352);

   stream_seek(self->sub_stream, lba * 96, SEEK_SET);
   stream_read(self->sub_stream, sub_buf, 96);

   subpw_interleave(sub_buf, buf + 2352);

   return true;
}

static bool CDAccess_CCD_Read_Raw_PW(CDAccess *base_self, uint8_t *buf,
      int32_t lba)
{
   CDAccess_CCD *self = (CDAccess_CCD *)base_self;
   uint8_t       sub_buf[96];

   if (lba < 0 || (size_t)lba >= self->img_numsectors)
   {
      MDFN_Error(0, "LBA out of range.");
      return false;
   }

   stream_seek(self->sub_stream, lba * 96, SEEK_SET);
   stream_read(self->sub_stream, sub_buf, 96);

   subpw_interleave(sub_buf, buf);

   return true;
}

static bool CDAccess_CCD_Read_TOC(CDAccess *base_self, TOC *toc)
{
   CDAccess_CCD *self = (CDAccess_CCD *)base_self;
   *toc = self->tocd;
   return true;
}

static void CDAccess_CCD_Eject(CDAccess *base_self, bool eject_status)
{
   (void)base_self;
   (void)eject_status;
}

static void CDAccess_CCD_destroy(CDAccess *base_self)
{
   CDAccess_CCD *self = (CDAccess_CCD *)base_self;
   CDAccess_CCD_Cleanup(self);
   free(self);
}

/* ------------------------------------------------------------------
 * Factory.
 * ------------------------------------------------------------------ */

extern "C" CDAccess *CDAccess_CCD_New(bool *success, const char *path,
      bool image_memcache)
{
   CDAccess_CCD *self = (CDAccess_CCD *)calloc(1, sizeof(*self));
   if (!self)
   {
      *success = false;
      return NULL;
   }

   self->base.Read_Raw_Sector = CDAccess_CCD_Read_Raw_Sector;
   self->base.Read_Raw_PW     = CDAccess_CCD_Read_Raw_PW;
   self->base.Read_TOC        = CDAccess_CCD_Read_TOC;
   self->base.Eject           = CDAccess_CCD_Eject;
   self->base.destroy         = CDAccess_CCD_destroy;

   self->img_stream     = NULL;
   self->sub_stream     = NULL;
   self->img_numsectors = 0;
   TOC_Clear(&self->tocd);

   if (!CDAccess_CCD_Load(self, path, image_memcache))
      *success = false;

   return &self->base;
}
