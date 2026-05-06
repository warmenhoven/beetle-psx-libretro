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
#include <map>
#include <string>

#include <mednafen/mednafen.h>
#include <mednafen/general.h>
#include <mednafen/mednafen-endian.h>
#include <mednafen/error.h>
#include <mednafen/FileStream.h>

#include "CDAccess.h"
#include "CDAccess_CHD.h"
#include "cdaccess_track.h"

#include <libchdr/chd.h>

extern retro_log_printf_t log_cb;

/* ------------------------------------------------------------------
 * Concrete struct - embeds CDAccess as the first member so that a
 * `CDAccess *` returned from the factory can be safely down-cast to
 * `CDAccess_CHD *` inside the vtable bodies.
 *
 * This file remains a .cpp because it still uses std::string and
 * std::map for SBI-path resolution and the SubQ replacement table;
 * those will move to plain C in a follow-up stage.
 * ------------------------------------------------------------------ */

struct CDAccess_CHD
{
   CDAccess     base;

   FileStream   file_stream;
   chd_file    *chd;
   uint8_t     *hunkmem;        /* hunk-data cache */
   int          oldhunk;        /* last hunknum read, -1 sentinel */

   int32_t      NumTracks;
   int32_t      FirstTrack;
   int32_t      LastTrack;
   int32_t      total_sectors;
   TOC         *ptoc;

   std::string  sbi_path;

   CDRFILE_TRACK_INFO Tracks[100];   /* Tracks #0 (HMM?) through 99 */

   std::map<uint32, cpp11_array_doodad> SubQReplaceMap;
};

/* Disk-image (rip) track/sector formats - kept file-static. */
enum
{
   DI_FORMAT_AUDIO       = 0x00,
   DI_FORMAT_MODE1       = 0x01,
   DI_FORMAT_MODE1_RAW   = 0x02,
   DI_FORMAT_MODE2       = 0x03,
   DI_FORMAT_MODE2_FORM1 = 0x04,
   DI_FORMAT_MODE2_FORM2 = 0x05,
   DI_FORMAT_MODE2_RAW   = 0x06,
   _DI_FORMAT_COUNT
};

/* libchdr file-IO callbacks - operate on a FileStream pointer. */

static uint64_t Callback_fsize(void *user_data)
{
   FileStream *file_stream = (FileStream *)user_data;
   return stream_size(&file_stream->base);
}

static size_t Callback_fread(void *buffer, size_t size, size_t count,
      void *user_data)
{
   FileStream *file_stream;
   if (size == 0 || count == 0)
      return 0;

   file_stream = (FileStream *)user_data;
   return stream_read(&file_stream->base, buffer, count * size) / size;
}

static int Callback_fclose(void *user_data)
{
   (void)user_data;
   return 0;
}

static int Callback_fseek(void *user_data, int64_t offset, int whence)
{
   FileStream *file_stream = (FileStream *)user_data;
   stream_seek(&file_stream->base, offset, whence);
   return 0;
}

static const chd_core_file_callbacks chd_callbacks =
{
   Callback_fsize,
   Callback_fread,
   Callback_fclose,
   Callback_fseek
};

/* ------------------------------------------------------------------
 * Body methods - take an explicit `CDAccess_CHD *self` first arg.
 * ------------------------------------------------------------------ */

static bool CDAccess_CHD_ImageOpen(CDAccess_CHD *self, const char *path,
      bool image_memcache)
{
   chd_error err = chd_open_core_file_callbacks(&chd_callbacks,
         &self->file_stream, CHD_OPEN_READ, NULL, &self->chd);
   if (err != CHDERR_NONE)
      return false;

   if (image_memcache)
   {
      err = chd_precache(self->chd);
      if (err != CHDERR_NONE)
         return false;
   }

   /* allocate storage for sector reads */
   const chd_header *head = chd_get_header(self->chd);
   self->hunkmem = (uint8_t *)malloc(head->hunkbytes);
   self->oldhunk = -1;

   log_cb(RETRO_LOG_INFO, "chd_load '%s' hunkbytes=%d\n", path,
         head->hunkbytes);

   int plba           = -150;
   uint32_t fileOffset = 0;

   char type[64], subtype[32], pgtype[32], pgsub[32];

   char meta_entry[256];
   uint32_t meta_entry_size = 0;

   while (1)
   {
      int tkid = 0, frames = 0, pad = 0, pregap = 0, postgap = 0;

      err = chd_get_metadata(self->chd, CDROM_TRACK_METADATA2_TAG,
            self->NumTracks, meta_entry, sizeof(meta_entry),
            &meta_entry_size, NULL, NULL);
      if (err == CHDERR_NONE)
      {
         sscanf(meta_entry, CDROM_TRACK_METADATA2_FORMAT,
               &tkid, type, subtype, &frames, &pregap, pgtype, pgsub,
               &postgap);
      }
      else
      {
         err = chd_get_metadata(self->chd, CDROM_TRACK_METADATA_TAG,
               self->NumTracks, meta_entry, sizeof(meta_entry),
               &meta_entry_size, NULL, NULL);
         if (err == CHDERR_NONE)
         {
            sscanf(meta_entry, CDROM_TRACK_METADATA_FORMAT,
                  &tkid, type, subtype, &frames);
         }
         else
            break;   /* end of TOC */
      }

      if (strncmp(type, "MODE2_RAW", 9) != 0
            && strncmp(type, "AUDIO", 5) != 0)
      {
         log_cb(RETRO_LOG_ERROR, "chd_parse track type %s unsupported\n",
               type);
         return false;
      }
      else if (strncmp(subtype, "NONE", 4) != 0)
      {
         log_cb(RETRO_LOG_ERROR, "chd_parse track subtype %s unsupported\n",
               subtype);
         return false;
      }

      /* add track */
      self->NumTracks++;

      if (self->NumTracks != tkid)
         log_cb(RETRO_LOG_WARN,
               "chd tracks are out of order, missing a track or contain a duplicate!\n");

      if (strncmp(type, "MODE2_RAW", 9) == 0)
      {
         self->Tracks[tkid].DIFormat      = DI_FORMAT_MODE2_RAW;
         self->Tracks[tkid].subq_control |= SUBQ_CTRLF_DATA;
      }
      else if (strncmp(type, "AUDIO", 5) == 0)
      {
         self->Tracks[tkid].DIFormat        = DI_FORMAT_AUDIO;
         self->Tracks[tkid].subq_control   &= ~SUBQ_CTRLF_DATA;
         self->Tracks[tkid].RawAudioMSBFirst = true;
      }

      self->Tracks[tkid].pregap    = (tkid == 1) ? 150
                                                 : (pgtype[0] == 'V') ? 0 : pregap;
      self->Tracks[tkid].pregap_dv = (pgtype[0] == 'V') ? pregap : 0;
      plba += self->Tracks[tkid].pregap + self->Tracks[tkid].pregap_dv;
      self->Tracks[tkid].LBA       = plba;
      self->Tracks[tkid].postgap   = postgap;
      self->Tracks[tkid].sectors   = frames - self->Tracks[tkid].pregap_dv;
      self->Tracks[tkid].SubchannelMode = 0;
      self->Tracks[tkid].index[0]  = -1;
      self->Tracks[tkid].index[1]  = 0;

      fileOffset                  += self->Tracks[tkid].pregap_dv;
      self->Tracks[tkid].FileOffset = fileOffset;
      fileOffset                  += frames - self->Tracks[tkid].pregap_dv;
      fileOffset                  += self->Tracks[tkid].postgap;
      fileOffset                  += ((frames + 3) & ~3) - frames;

      plba += frames - self->Tracks[tkid].pregap_dv;
      plba += self->Tracks[tkid].postgap;

      self->total_sectors += (tkid == 1)
            ? frames
            : frames + self->Tracks[tkid].pregap;

      if (tkid < self->FirstTrack)
         self->FirstTrack = tkid;
      if (tkid > self->LastTrack)
         self->LastTrack = tkid;
   }

   /* prepare sbi file path */
   {
      std::string base_dir, file_base, file_ext;
      char sbi_ext[4] = { 's', 'b', 'i', 0 };

      MDFN_GetFilePathComponents(path, &base_dir, &file_base, &file_ext);

      if (file_ext.length() == 4 && file_ext[0] == '.')
      {
         for (int i = 0; i < 3; i++)
         {
            if (file_ext[1 + i] >= 'A' && file_ext[1 + i] <= 'Z')
               sbi_ext[i] += 'A' - 'a';
         }
      }
      self->sbi_path = MDFN_EvalFIP(base_dir,
            file_base + std::string(".") + std::string(sbi_ext));
   }

   return true;
}

static void CDAccess_CHD_Cleanup(CDAccess_CHD *self)
{
   if (self->chd)
      chd_close(self->chd);

   if (self->hunkmem)
      free(self->hunkmem);

   /* file_stream is an in-place FileStream - close it (NOT destroy). */
   stream_close(&self->file_stream.base);
}

/* MakeSubPQ ORs the simulated P and Q subchannel data into SubPWBuf. */
static int32_t CDAccess_CHD_MakeSubPQ(CDAccess_CHD *self, int32_t lba,
      uint8_t *SubPWBuf)
{
   unsigned i;
   uint8_t  buf[0xC];
   uint8_t  adr, control;
   int32_t  track;
   uint32_t lba_relative;
   uint32_t ma, sa, fa;
   uint32_t m, s, f;
   uint8_t  pause_or = 0x00;
   bool     track_found = false;

   for (track = self->FirstTrack;
         track < (self->FirstTrack + self->NumTracks); track++)
   {
      if (lba >= (self->Tracks[track].LBA - self->Tracks[track].pregap_dv
                  - self->Tracks[track].pregap)
            && lba < (self->Tracks[track].LBA + self->Tracks[track].sectors
                      + self->Tracks[track].postgap))
      {
         track_found = true;
         break;
      }
   }

   if (!track_found)
      track = self->FirstTrack;

   lba_relative = abs((int32_t)lba - self->Tracks[track].LBA);

   f = (lba_relative % 75);
   s = ((lba_relative / 75) % 60);
   m = (lba_relative / 75 / 60);

   fa = (lba + 150) % 75;
   sa = ((lba + 150) / 75) % 60;
   ma = ((lba + 150) / 75 / 60);

   adr     = 0x1;   /* Q channel data encodes position */
   control = self->Tracks[track].subq_control;

   /* Pause bit (D7) - set when in pregap or postgap. */
   if (lba < self->Tracks[track].LBA
         || lba >= self->Tracks[track].LBA + self->Tracks[track].sectors)
      pause_or = 0x80;

   /* Pregap between audio->data track. */
   {
      int32_t pg_offset = (int32_t)lba - self->Tracks[track].LBA;

      if (pg_offset < -150)
      {
         if ((self->Tracks[track].subq_control & SUBQ_CTRLF_DATA)
               && (self->FirstTrack < track)
               && !(self->Tracks[track - 1].subq_control & SUBQ_CTRLF_DATA))
            control = self->Tracks[track - 1].subq_control;
      }
   }

   memset(buf, 0, 0xC);
   buf[0] = (adr << 0) | (control << 4);
   buf[1] = U8_to_BCD(track);

   if (lba < self->Tracks[track].LBA)   /* Index is 00 in pregap */
      buf[2] = U8_to_BCD(0x00);
   else
      buf[2] = U8_to_BCD(0x01);

   /* Track-relative MSF address */
   buf[3] = U8_to_BCD(m);
   buf[4] = U8_to_BCD(s);
   buf[5] = U8_to_BCD(f);
   buf[6] = 0;
   /* Absolute MSF address */
   buf[7] = U8_to_BCD(ma);
   buf[8] = U8_to_BCD(sa);
   buf[9] = U8_to_BCD(fa);

   subq_generate_checksum(buf);

   if (!self->SubQReplaceMap.empty())
   {
      std::map<uint32, cpp11_array_doodad>::const_iterator it
         = self->SubQReplaceMap.find(LBA_to_ABA(lba));

      if (it != self->SubQReplaceMap.end())
         memcpy(buf, it->second.data, 12);
   }

   for (i = 0; i < 96; i++)
      SubPWBuf[i] |= (((buf[i >> 3] >> (7 - (i & 0x7))) & 1) ? 0x40 : 0x00)
                     | pause_or;

   return track;
}

static bool CDAccess_CHD_Read_Raw_Sector(CDAccess *base_self, uint8_t *buf,
      int32_t lba)
{
   CDAccess_CHD *self = (CDAccess_CHD *)base_self;
   uint8_t  SimuQ[0xC];
   int32_t  track;
   CDRFILE_TRACK_INFO *ct;

   /* Leadout synthesis */
   if (lba >= self->total_sectors)
   {
      uint8_t data_synth_mode = 0x01;
      switch (self->Tracks[self->LastTrack].DIFormat)
      {
         case DI_FORMAT_AUDIO:
            break;
         case DI_FORMAT_MODE1_RAW:
         case DI_FORMAT_MODE1:
            data_synth_mode = 0x01;
            break;
         case DI_FORMAT_MODE2_RAW:
         case DI_FORMAT_MODE2_FORM1:
         case DI_FORMAT_MODE2_FORM2:
         case DI_FORMAT_MODE2:
            data_synth_mode = 0x02;
            break;
      }
      synth_leadout_sector_lba(data_synth_mode, self->ptoc, lba, buf);
   }

   memset(buf + 2352, 0, 96);
   track = CDAccess_CHD_MakeSubPQ(self, lba, buf + 2352);
   subq_deinterleave(buf + 2352, SimuQ);

   ct = &self->Tracks[track];

   /* Pregap and postgap synthesis */
   if (lba < (ct->LBA - ct->pregap_dv) || lba >= (ct->LBA + ct->sectors))
   {
      int32_t pg_offset = lba - ct->LBA;
      CDRFILE_TRACK_INFO *et = ct;

      if (pg_offset < -150)
      {
         if ((self->Tracks[track].subq_control & SUBQ_CTRLF_DATA)
               && (self->FirstTrack < track)
               && !(self->Tracks[track - 1].subq_control & SUBQ_CTRLF_DATA))
            et = &self->Tracks[track - 1];
      }

      memset(buf, 0, 2352);
      switch (et->DIFormat)
      {
         case DI_FORMAT_AUDIO:
            break;
         case DI_FORMAT_MODE1_RAW:
         case DI_FORMAT_MODE1:
            encode_mode1_sector(lba + 150, buf);
            break;
         case DI_FORMAT_MODE2_RAW:
         case DI_FORMAT_MODE2_FORM1:
         case DI_FORMAT_MODE2_FORM2:
         case DI_FORMAT_MODE2:
            buf[12 + 6]  = 0x20;
            buf[12 + 10] = 0x20;
            encode_mode2_form2_sector(lba + 150, buf);
            break;
      }
   }
   else
   {
      const chd_header *head = chd_get_header(self->chd);
      int cad     = lba - ct->LBA + ct->FileOffset;
      int sph     = head->hunkbytes / (2352 + 96);
      int hunknum = cad / sph;
      int hunkofs = cad % sph;
      int err     = CHDERR_NONE;

      /* Each hunk holds ~8 sectors; cache the most-recently-read one. */
      if (hunknum != self->oldhunk)
      {
         err = chd_read(self->chd, hunknum, self->hunkmem);
         if (err != CHDERR_NONE)
            log_cb(RETRO_LOG_ERROR,
                  "chd_read_sector failed lba=%d error=%d\n", lba, err);
         else
            self->oldhunk = hunknum;
      }

      memcpy(buf, self->hunkmem + hunkofs * (2352 + 96), 2352);

      if (ct->DIFormat == DI_FORMAT_AUDIO && ct->RawAudioMSBFirst)
         Endian_A16_Swap(buf, 588 * 2);
   }
   return true;
}

static bool CDAccess_CHD_Read_Raw_PW(CDAccess *base_self, uint8_t *buf,
      int32_t lba)
{
   CDAccess_CHD *self = (CDAccess_CHD *)base_self;
   memset(buf, 0, 96);
   CDAccess_CHD_MakeSubPQ(self, lba, buf);
   return true;
}

static int CDAccess_CHD_LoadSBI(CDAccess_CHD *self, const char *sbi_path);

static bool CDAccess_CHD_Read_TOC(CDAccess *base_self, TOC *toc)
{
   CDAccess_CHD *self = (CDAccess_CHD *)base_self;
   int i;

   TOC_Clear(toc);

   toc->first_track = self->FirstTrack;
   toc->last_track  = self->LastTrack;
   toc->disc_type   = DISC_TYPE_CD_XA;

   for (i = 1; i <= self->NumTracks; i++)
   {
      toc->tracks[i].control = self->Tracks[i].subq_control;
      toc->tracks[i].adr     = ADR_CURPOS;
      toc->tracks[i].lba     = self->Tracks[i].LBA;
   }

   toc->tracks[100].lba     = self->total_sectors;
   toc->tracks[100].adr     = ADR_CURPOS;
   toc->tracks[100].control = toc->tracks[toc->last_track].control & 0x4;

   /* Convenience leadout track duplication. */
   if (toc->last_track < 99)
      toc->tracks[toc->last_track + 1] = toc->tracks[100];

   if (!self->SubQReplaceMap.empty())
      self->SubQReplaceMap.clear();

   /* Load SBI file, if present. */
   if (filestream_exists(self->sbi_path.c_str()))
      CDAccess_CHD_LoadSBI(self, self->sbi_path.c_str());

   self->ptoc = toc;
   log_cb(RETRO_LOG_INFO, "chd_read_toc: finished\n");
   return true;
}

static int CDAccess_CHD_LoadSBI(CDAccess_CHD *self, const char *sbi_path)
{
   /* All return paths past mdfn_filestream_init pass through cleanup:
    * which closes the in-place FileStream. */
   uint8_t header[4];
   uint8_t ed[4 + 10];
   uint8_t tmpq[12];
   FileStream sbis;
   int ret = 0;

   mdfn_filestream_init(&sbis, sbi_path);

   stream_read(&sbis.base, header, 4);

   if (memcmp(header, "SBI\0", 4))
   {
      ret = -1;
      goto cleanup;
   }

   while (stream_read(&sbis.base, ed, sizeof(ed)) == sizeof(ed))
   {
      if (!BCD_is_valid(ed[0]) || !BCD_is_valid(ed[1])
            || !BCD_is_valid(ed[2]))
      {
         ret = -1;
         goto cleanup;
      }

      if (ed[3] != 0x01)
      {
         ret = -1;
         goto cleanup;
      }

      memcpy(tmpq, &ed[4], 10);

      subq_generate_checksum(tmpq);
      tmpq[10] ^= 0xFF;
      tmpq[11] ^= 0xFF;

      {
         uint32_t aba = AMSF_to_ABA(BCD_to_U8(ed[0]),
               BCD_to_U8(ed[1]), BCD_to_U8(ed[2]));
         memcpy(self->SubQReplaceMap[aba].data, tmpq, 12);
      }
   }

   log_cb(RETRO_LOG_INFO, "[CHD] Loaded SBI file %s\n", sbi_path);
cleanup:
   stream_close(&sbis.base);
   return ret;
}

static void CDAccess_CHD_Eject(CDAccess *base_self, bool eject_status)
{
   (void)base_self;
   (void)eject_status;
}

static void CDAccess_CHD_destroy(CDAccess *base_self)
{
   CDAccess_CHD *self = (CDAccess_CHD *)base_self;
   CDAccess_CHD_Cleanup(self);
   /* Manually call destructors for the C++ members embedded in self
    * (std::string, std::map) - these were created by `new` in the
    * factory below.  free() the memory afterwards. */
   self->sbi_path.~basic_string();
   self->SubQReplaceMap.~map();
   free(self);
}

/* ------------------------------------------------------------------
 * Factory.
 * ------------------------------------------------------------------ */

extern "C" CDAccess *CDAccess_CHD_New(bool *success, const char *path,
      bool image_memcache)
{
   /* Allocate raw and use placement-new to construct the C++ members. */
   CDAccess_CHD *self = (CDAccess_CHD *)calloc(1, sizeof(*self));
   if (!self)
   {
      *success = false;
      return NULL;
   }

   /* Placement-new on the std::string and std::map members.  The
    * destroy() vtable entry will invoke their destructors. */
   new (&self->sbi_path) std::string();
   new (&self->SubQReplaceMap) std::map<uint32, cpp11_array_doodad>();

   /* Vtable */
   self->base.Read_Raw_Sector = CDAccess_CHD_Read_Raw_Sector;
   self->base.Read_Raw_PW     = CDAccess_CHD_Read_Raw_PW;
   self->base.Read_TOC        = CDAccess_CHD_Read_TOC;
   self->base.Eject           = CDAccess_CHD_Eject;
   self->base.destroy         = CDAccess_CHD_destroy;

   self->chd           = NULL;
   self->NumTracks     = 0;
   self->total_sectors = 0;
   self->FirstTrack    = 99;   /* opposites for min/max init */
   self->LastTrack     = 0;

   /* file_stream is in-place FileStream; init in place. */
   mdfn_filestream_init(&self->file_stream, path);

   if (!mdfn_filestream_is_open(&self->file_stream))
   {
      MDFN_Error(0, "CHD: failed to open \"%s\"", path);
      *success = false;
      return &self->base;
   }

   if (!CDAccess_CHD_ImageOpen(self, path, image_memcache))
      *success = false;

   return &self->base;
}
