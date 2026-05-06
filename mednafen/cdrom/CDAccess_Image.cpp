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

/*
 Notes and TODO:

	POSTGAP in CUE sheets may not be handled properly, should the directive automatically increment the index number?

	INDEX nn where 02 <= nn <= 99 is not supported in CUE sheets.

	TOC reading code is extremely barebones, leaving out support for more esoteric features.

	A PREGAP statement in the first track definition in a CUE sheet may not work properly(depends on what is proper);
	it will be added onto the implicit default 00:02:00 of pregap.

	Trying to read sectors at an LBA of less than 0 is not supported.  TODO: support it(at least up to -150).
*/

#include <new>
#include <boolean.h>
#include <streams/file_stream.h>

#include "../mednafen.h"
#include "../error.h"

#include <sys/types.h>

#include <string.h>
#include <time.h>

#include "../general.h"
#include "../FileStream.h"
#include "../MemoryStream.h"

#include "CDAccess.h"
#include "misc.h"
#include "cdaccess_track.h"
#include "CDAccess_Image.h"
#include "CDUtility.h"

#include "audioreader.h"

#include <libretro.h>

extern retro_log_printf_t log_cb;

#include <map>

enum
{
   CDRF_SUBM_NONE = 0,
   CDRF_SUBM_RW,
   CDRF_SUBM_RW_RAW
};

// Disk-image(rip) track/sector formats
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

static const int32 DI_Size_Table[7] =
{
   2352, // Audio
   2048, // MODE1
   2352, // MODE1 RAW
   2336, // MODE2
   2048, // MODE2 Form 1
   2324, // Mode 2 Form 2
   2352
};

static const char *DI_CDRDAO_Strings[7] = 
{
   "AUDIO",
   "MODE1",
   "MODE1_RAW",
   "MODE2",
   "MODE2_FORM1",
   "MODE2_FORM2",
   "MODE2_RAW"
};

static const char *DI_CUE_Strings[7] = 
{
   "AUDIO",
   "MODE1/2048",
   "MODE1/2352",

   // FIXME: These are just guesses:
   "MODE2/2336",
   "MODE2/2048",
   "MODE2/2324",
   "MODE2/2352"
};

static void Endian_A16_Swap(void *src, uint32_t nelements)
{
   uint32_t i;
   uint8_t *nsrc = (uint8_t *)src;

   for(i = 0; i < nelements; i++)
   {
      uint8_t tmp = nsrc[i * 2];

      nsrc[i * 2] = nsrc[i * 2 + 1];
      nsrc[i * 2 + 1] = tmp;
   }
}

static inline void MDFN_en16lsb(uint8_t *buf, uint16_t morp)
{
   buf[0]=morp;
   buf[1]=morp>>8;
}

// Should return an offset to the start of the next argument(past any whitespace), or if there isn't a next argument,
// it'll return the length of the src string.
static size_t UnQuotify(const std::string &src, size_t source_offset,
      std::string &dest, bool parse_quotes = true)
{
   const size_t source_len = src.length();
   bool in_quote = 0;
   bool already_normal = 0;

   dest.clear();

   while(source_offset < source_len)
   {
      if(src[source_offset] == ' ' || src[source_offset] == '\t')
      {
         if(!in_quote)
         {
            if(already_normal)	// Trailing whitespace(IE we're done with this argument)
               break;
            else		// Leading whitespace, ignore it.
            {
               source_offset++;
               continue;
            }
         }
      }

      if(src[source_offset] == '"' && parse_quotes)
      {
         if(in_quote)
         {
            source_offset++;
            // Not sure which behavior is most useful(or correct :b).
#if 0
            in_quote = false;
            already_normal = true;
#else
            break;
#endif
         }
         else
            in_quote = 1;
      }
      else
      {
         dest.push_back(src[source_offset]);
         already_normal = 1;
      }
      source_offset++;
   }

   while(source_offset < source_len)
   {
      if(src[source_offset] != ' ' && src[source_offset] != '\t')
         break;

      source_offset++;
   }

   return source_offset;
}

struct CDAccess_Image
{
   CDAccess  base;

   int32_t   NumTracks;
   int32_t   FirstTrack;
   int32_t   LastTrack;
   int32_t   total_sectors;
   uint8_t   disc_type;
   CDRFILE_TRACK_INFO Tracks[100];

   std::map<uint32, cpp11_array_doodad> SubQReplaceMap;
   std::string base_dir;
};

/* Forward declarations - methods reference each other regardless of
 * source order. */
static uint32 CDAccess_Image_GetSectorCount(CDAccess_Image *self, CDRFILE_TRACK_INFO *track);
static bool CDAccess_Image_ParseTOCFileLineInfo(CDAccess_Image *self, CDRFILE_TRACK_INFO *track, const int tracknum, const std::string &filename, const char *binoffset, const char *msfoffset, const char *length, bool image_memcache, std::map<std::string, Stream*> &toc_streamcache);
static int CDAccess_Image_LoadSBI(CDAccess_Image *self, const char* sbi_path);
static bool CDAccess_Image_ImageOpen(CDAccess_Image *self, const char *path, bool image_memcache);
static void CDAccess_Image_Cleanup(CDAccess_Image *self);
static void CDAccess_Image_MakeSubPQ(CDAccess_Image *self, int32 lba, uint8 *SubPWBuf);



static uint32 CDAccess_Image_GetSectorCount(CDAccess_Image *self, CDRFILE_TRACK_INFO *track){
   int64 size;

   if(track->DIFormat == DI_FORMAT_AUDIO)
   {
      if(track->AReader)
         return(((track->AReader->FrameCount() * 4) - track->FileOffset) / 2352);

      size = stream_size(track->fp);

      if(track->SubchannelMode)
         return((size - track->FileOffset) / (2352 + 96));
      return((size - track->FileOffset) / 2352);
   }

   size = stream_size(track->fp);

   return((size - track->FileOffset) / DI_Size_Table[track->DIFormat]);
}

static bool CDAccess_Image_ParseTOCFileLineInfo(CDAccess_Image *self, CDRFILE_TRACK_INFO *track, const int tracknum,
      const std::string &filename, const char *binoffset, const char *msfoffset,
      const char *length, bool image_memcache, std::map<std::string, Stream*> &toc_streamcache){
   long offset = 0; // In bytes!
   long tmp_long;
   int m, s, f;
   uint32 sector_mult;
   long sectors;
   std::map<std::string, Stream*>::iterator ribbit;

   ribbit = toc_streamcache.find(filename);

   if(ribbit != toc_streamcache.end())
   {
      track->FirstFileInstance = 0;

      track->fp = ribbit->second;
   }
   else
   {
      std::string efn;

      track->FirstFileInstance = 1;

      efn = MDFN_EvalFIP(self->base_dir, filename);

      if(image_memcache)
      {
         FileStream *file = mdfn_filestream_new(efn.c_str());
         if (!mdfn_filestream_is_open(file))
         {
            MDFN_Error(0, "Could not open track file \"%s\"", efn.c_str());
            if (file)
               stream_destroy(&file->base);
            return false;
         }
         /* mdfn_memstream_new_from_stream consumes &file->base regardless
          * of success - no further cleanup of `file` required. */
         {
            struct MemoryStream *mem = mdfn_memstream_new_from_stream(&file->base);
            if (!mdfn_memstream_is_valid(mem))
            {
               if (mem)
                  stream_destroy(&mem->base);
               track->fp = NULL;
               return false;
            }
            track->fp = &mem->base;
         }
      }
      else
      {
         FileStream *file = mdfn_filestream_new(efn.c_str());
         if (!mdfn_filestream_is_open(file))
         {
            MDFN_Error(0, "Could not open track file \"%s\"", efn.c_str());
            if (file)
               stream_destroy(&file->base);
            return false;
         }
         track->fp = &file->base;
      }

      toc_streamcache[filename] = track->fp;
   }

   if(filename.length() >= 4 && !strcasecmp(filename.c_str() + filename.length() - 4, ".wav"))
   {
      track->AReader = AR_Open(track->fp);

      if(!track->AReader)
      {
         MDFN_Error(0, "Failed to open audio track \"%s\" as Ogg Vorbis", filename.c_str());
         return false;
      }
   }

   sector_mult = DI_Size_Table[track->DIFormat];

   if(track->SubchannelMode)
      sector_mult += 96;

   if(binoffset && sscanf(binoffset, "%ld", &tmp_long) == 1)
   {
      offset += tmp_long;
   }

   if(msfoffset && sscanf(msfoffset, "%d:%d:%d", &m, &s, &f) == 3)
   {
      offset += ((m * 60 + s) * 75 + f) * sector_mult;
   }

   track->FileOffset = offset; // Make sure this is set before calling CDAccess_Image_GetSectorCount(self)!
   sectors = CDAccess_Image_GetSectorCount(self, track);

   if(length)
   {
      tmp_long = sectors;

      if(sscanf(length, "%d:%d:%d", &m, &s, &f) == 3)
         tmp_long = (m * 60 + s) * 75 + f;
      else if(track->DIFormat == DI_FORMAT_AUDIO)
      {
         char *endptr = NULL;

         tmp_long = strtol(length, &endptr, 10);

         // Error?
         if(endptr == length)
         {
            tmp_long = sectors;
         }
         else
            tmp_long /= 588;

      }

      if(tmp_long > sectors)
      {
         MDFN_Error(0, "Length specified in TOC file for track %d is too large by %ld sectors!\n", tracknum, (long)(tmp_long - sectors));
         return false;
      }
      sectors = tmp_long;
   }

   track->sectors = sectors;
   return true;
}

static int CDAccess_Image_LoadSBI(CDAccess_Image *self, const char* sbi_path){
   /* Loading SBI file */
   uint8 header[4];
   uint8 ed[4 + 10];
   uint8 tmpq[12];
   RFILE *sbis      = filestream_open(sbi_path, 
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!sbis)
      return -1;

   filestream_read(sbis, header, 4);

   if(memcmp(header, "SBI\0", 4))
      goto error;

   while(filestream_read(sbis, ed, sizeof(ed)) == sizeof(ed))
   {
      /* Bad BCD MSF offset in SBI file. */
      if(!BCD_is_valid(ed[0]) || !BCD_is_valid(ed[1]) || !BCD_is_valid(ed[2]))
         goto error;

      /* Unrecognized boogly oogly in SBI file */
      if(ed[3] != 0x01)
         goto error;

      memcpy(tmpq, &ed[4], 10);

      subq_generate_checksum(tmpq);
      tmpq[10] ^= 0xFF;
      tmpq[11] ^= 0xFF;

      uint32 aba = AMSF_to_ABA(BCD_to_U8(ed[0]), BCD_to_U8(ed[1]), BCD_to_U8(ed[2]));

      memcpy(self->SubQReplaceMap[aba].data, tmpq, 12);
   }

   log_cb(RETRO_LOG_INFO, "[Image] Loaded SBI file %s\n", sbi_path);
   filestream_close(sbis);
   return 0;

error:
   if (sbis)
      filestream_close(sbis);
   return -1;
}

static bool CDAccess_Image_ImageOpen(CDAccess_Image *self, const char *path, bool image_memcache){
   FileStream *probe = mdfn_filestream_new(path);
   if (!mdfn_filestream_is_open(probe))
   {
      MDFN_Error(0, "Could not open \"%s\"", path);
      if (probe)
         stream_destroy(&probe->base);
      return false;
   }

   /* Stack-local MemoryStream that slurps the probe; mdfn_memstream_init_
    * from_stream consumes &probe->base. fp must be cleaned up via
    * stream_close (NOT stream_destroy - it's a stack address). All
    * failure paths in the body below set ok=false and fall through to
    * the single cleanup at the bottom rather than returning early. */
   struct MemoryStream fp;
   bool ok = true;
   /* Hoisted from mid-function so the `goto cleanup` paths above don't
    * cross their initialization (illegal in C++). They were locals to
    * the post-parse track-fixup loop. */
   int32 RunningLBA = 0;
   int32 LastIndex  = 0;
   long  FileOffset = 0;
   /* Silence GCC warning - LastIndex is assigned but only conditionally read */
   (void)LastIndex;

   mdfn_memstream_init_from_stream(&fp, &probe->base);
   if (!mdfn_memstream_is_valid(&fp))
   {
      MDFN_Error(0, "Could not load \"%s\" into memory", path);
      stream_close(&fp.base);
      return false;
   }

   static const unsigned max_args = 4;
   std::string linebuf;
   std::string cmdbuf, args[max_args];
   bool IsTOC = false;
   int32 active_track = -1;
   int32 AutoTrackInc = 1; // For TOC
   CDRFILE_TRACK_INFO TmpTrack;
   std::string file_base, file_ext;
   std::map<std::string, Stream*> toc_streamcache;

   self->disc_type = DISC_TYPE_CDDA_OR_M1;
   memset(&TmpTrack, 0, sizeof(TmpTrack));

   MDFN_GetFilePathComponents(path, &self->base_dir, &file_base, &file_ext);

   if(!strcasecmp(file_ext.c_str(), ".toc"))
   {
      log_cb(RETRO_LOG_INFO, "TOC file detected.\n");
      IsTOC = true;
   }

   // Check for annoying UTF-8 BOM.
   if(!IsTOC)
   {
      uint8 bom_tmp[3];

      if(stream_read(&fp.base, bom_tmp, 3) == 3 && bom_tmp[0] == 0xEF && bom_tmp[1] == 0xBB && bom_tmp[2] == 0xBF)
      {
         log_cb(RETRO_LOG_ERROR, "UTF-8 BOM detected at start of CUE sheet.\n");
      }
      else
         stream_seek(&fp.base, 0, SEEK_SET);
   }


   // Assign opposite maximum values so our tests will work!
   self->FirstTrack = 99;
   self->LastTrack = 0;

   linebuf.reserve(1024);
   while(stream_get_line_string(&fp.base, linebuf) >= 0)
   {
      unsigned argcount = 0;

      if(IsTOC)
      {
         // Handle TOC format comments
         size_t ss_loc = linebuf.find("//");

         if(ss_loc != std::string::npos)
            linebuf.resize(ss_loc);
      }

      // Call trim AFTER we handle TOC-style comments, so we'll be sure to remove trailing whitespace in lines like: MONKEY  // BABIES
      MDFN_trim(linebuf);

      if(linebuf.length() == 0)	// Skip blank lines.
         continue;

      // Grab command and arguments.
      {
         size_t offs = 0;

         offs = UnQuotify(linebuf, offs, cmdbuf, false);
         for(argcount = 0; argcount < max_args && offs < linebuf.length(); argcount++)
            offs = UnQuotify(linebuf, offs, args[argcount]);

         // Make sure unused arguments are cleared out so we don't have inter-line leaks!
         for(unsigned x = argcount; x < max_args; x++)
            args[x].clear();

         MDFN_strtoupper(cmdbuf);
      }

      if(IsTOC)
      {
         if(cmdbuf == "TRACK")
         {
            if(active_track >= 0)
            {
               memcpy(&self->Tracks[active_track], &TmpTrack, sizeof(TmpTrack));
               memset(&TmpTrack, 0, sizeof(TmpTrack));
               active_track = -1;
            }

            if(AutoTrackInc > 99)
            {
               MDFN_Error(0, "Invalid track number: %d", AutoTrackInc);
               { ok = false; goto cleanup; }
            }

            active_track = AutoTrackInc++;
            if(active_track < self->FirstTrack)
               self->FirstTrack = active_track;
            if(active_track > self->LastTrack)
               self->LastTrack = active_track;

            int format_lookup;
            for(format_lookup = 0; format_lookup < _DI_FORMAT_COUNT; format_lookup++)
            {
               if(!strcasecmp(args[0].c_str(), DI_CDRDAO_Strings[format_lookup]))
               {
                  TmpTrack.DIFormat = format_lookup;
                  break;
               }
            }

            if(format_lookup == _DI_FORMAT_COUNT)
            {
               MDFN_Error(0, "Invalid track format: %s", args[0].c_str());
               { ok = false; goto cleanup; }
            }

            if(TmpTrack.DIFormat == DI_FORMAT_AUDIO)
               TmpTrack.RawAudioMSBFirst = true; // Silly cdrdao...

            if(!strcasecmp(args[1].c_str(), "RW"))
            {
               TmpTrack.SubchannelMode = CDRF_SUBM_RW;
               MDFN_Error(0, "\"RW\" format subchannel data not supported, only \"RW_RAW\" is!");
               { ok = false; goto cleanup; }
            }
            else if(!strcasecmp(args[1].c_str(), "RW_RAW"))
               TmpTrack.SubchannelMode = CDRF_SUBM_RW_RAW;

         } // end to TRACK
         else if(cmdbuf == "SILENCE")
         {
            //throw MDFN_Error(0, "Unsupported directive: %s", cmdbuf.c_str());
         }
         else if(cmdbuf == "ZERO")
         {
            //throw MDFN_Error(0, "Unsupported directive: %s", cmdbuf.c_str());
         }
         else if(cmdbuf == "FIFO")
         {
            MDFN_Error(0, "Unsupported directive: %s", cmdbuf.c_str());
            { ok = false; goto cleanup; }
         }
         else if(cmdbuf == "FILE" || cmdbuf == "AUDIOFILE")
         {
            const char *binoffset = NULL;
            const char *msfoffset = NULL;
            const char *length = NULL;

            if(args[1].c_str()[0] == '#')
            {
               binoffset = args[1].c_str() + 1;
               msfoffset = args[2].c_str();
               length = args[3].c_str();
            }
            else
            {
               msfoffset = args[1].c_str();
               length = args[2].c_str();
            }
            CDAccess_Image_ParseTOCFileLineInfo(self, &TmpTrack, active_track, args[0], binoffset, msfoffset, length, image_memcache, toc_streamcache);
         }
         else if(cmdbuf == "DATAFILE")
         {
            const char *binoffset = NULL;
            const char *length = NULL;

            if(args[1].c_str()[0] == '#') 
            {
               binoffset = args[1].c_str() + 1;
               length = args[2].c_str();
            }
            else
               length = args[1].c_str();

            CDAccess_Image_ParseTOCFileLineInfo(self, &TmpTrack, active_track, args[0], binoffset, NULL, length, image_memcache, toc_streamcache);
         }
         else if(cmdbuf == "INDEX")
         {

         }
         else if(cmdbuf == "PREGAP")
         {
            if(active_track < 0)
            {
               MDFN_Error(0, "Command %s is outside of a TRACK definition!\n", cmdbuf.c_str());
               { ok = false; goto cleanup; }
            }
            int m,s,f;
            sscanf(args[0].c_str(), "%d:%d:%d", &m, &s, &f);
            TmpTrack.pregap = (m * 60 + s) * 75 + f;
         } // end to PREGAP
         else if(cmdbuf == "START")
         {
            if(active_track < 0)
            {
               MDFN_Error(0, "Command %s is outside of a TRACK definition!\n", cmdbuf.c_str());
               { ok = false; goto cleanup; }
            }
            int m,s,f;
            sscanf(args[0].c_str(), "%d:%d:%d", &m, &s, &f);
            TmpTrack.pregap = (m * 60 + s) * 75 + f;
         }
         else if(cmdbuf == "TWO_CHANNEL_AUDIO")
         {
            TmpTrack.subq_control &= ~SUBQ_CTRLF_4CH;
         }
         else if(cmdbuf == "FOUR_CHANNEL_AUDIO")
         {
            TmpTrack.subq_control |= SUBQ_CTRLF_4CH;
         }
         else if(cmdbuf == "NO")
         {
            MDFN_strtoupper(args[0]);

            if(args[0] == "COPY")
            {
               TmpTrack.subq_control &= ~SUBQ_CTRLF_DCP; 
            }
            else if(args[0] == "PRE_EMPHASIS")
            {
               TmpTrack.subq_control &= ~SUBQ_CTRLF_PRE;
            }
            else
            {
               MDFN_Error(0, "Unsupported argument to \"NO\" directive: %s", args[0].c_str());
               { ok = false; goto cleanup; }
            }
         }
         else if(cmdbuf == "COPY")
         {
            TmpTrack.subq_control |= SUBQ_CTRLF_DCP;
         } 
         else if(cmdbuf == "PRE_EMPHASIS")
         {
            TmpTrack.subq_control |= SUBQ_CTRLF_PRE;
         }
         // TODO: Confirm that these are taken from the TOC of the disc, and not synthesized by cdrdao.
         else if(cmdbuf == "CD_DA")
            self->disc_type = DISC_TYPE_CDDA_OR_M1;
         else if(cmdbuf == "CD_ROM")
            self->disc_type = DISC_TYPE_CDDA_OR_M1;
         else if(cmdbuf == "CD_ROM_XA")
            self->disc_type = DISC_TYPE_CD_XA;
         else
         {
            //throw MDFN_Error(0, "Unsupported directive: %s", cmdbuf.c_str());
         }
         // TODO: CATALOG

      } /*********** END TOC HANDLING ************/
      else // now for CUE sheet handling
      {
         if(cmdbuf == "FILE")
         {
            if(active_track >= 0)
            {
               memcpy(&self->Tracks[active_track], &TmpTrack, sizeof(TmpTrack));
               memset(&TmpTrack, 0, sizeof(TmpTrack));
               active_track = -1;
            }

            std::string efn;

            if(args[0].find("cdrom://") == std::string::npos)
               efn = MDFN_EvalFIP(self->base_dir, args[0]);
            else
               efn = args[0];

            {
               FileStream *probe = mdfn_filestream_new(efn.c_str());
               if (!mdfn_filestream_is_open(probe))
               {
                  MDFN_Error(0, "Could not open track file \"%s\"", efn.c_str());
                  if (probe)
                     stream_destroy(&probe->base);
                  { ok = false; goto cleanup; }
               }
               TmpTrack.fp = &probe->base;
            }
            TmpTrack.FirstFileInstance = 1;

            if(image_memcache)
            {
               struct MemoryStream *mem = mdfn_memstream_new_from_stream(TmpTrack.fp);
               /* mdfn_memstream_new_from_stream consumes its argument
                * regardless of success; on alloc failure mem is NULL.
                * In that case TmpTrack.fp is the now-dangling old
                * pointer - clear it so we don't double-free at cleanup. */
               TmpTrack.fp = mem ? &mem->base : NULL;
            }

            if(!strcasecmp(args[1].c_str(), "BINARY"))
            {
               //TmpTrack.Format = TRACK_FORMAT_DATA;
               //struct stat stat_buf;
               //fstat(fileno(TmpTrack.fp), &stat_buf);
               //TmpTrack.sectors = stat_buf.st_size; // / 2048;
            }
            else if(!strcasecmp(args[1].c_str(), "OGG") || !strcasecmp(args[1].c_str(), "VORBIS") || !strcasecmp(args[1].c_str(), "WAVE") || !strcasecmp(args[1].c_str(), "WAV") || !strcasecmp(args[1].c_str(), "PCM")
                  || !strcasecmp(args[1].c_str(), "MPC") || !strcasecmp(args[1].c_str(), "MP+"))
            {
               TmpTrack.AReader = AR_Open(TmpTrack.fp);

               if(!TmpTrack.AReader)
               {
                  MDFN_Error(0, "Unsupported audio track file format: %s\n", args[0].c_str());
                  { ok = false; goto cleanup; }
               }
            }
            else
            {
               MDFN_Error(0, "Unsupported track format: %s\n", args[1].c_str());
               { ok = false; goto cleanup; }
            }
         }
         else if(cmdbuf == "TRACK")
         {
            if(active_track >= 0)
            {
               memcpy(&self->Tracks[active_track], &TmpTrack, sizeof(TmpTrack));
               TmpTrack.FirstFileInstance = 0;
               TmpTrack.pregap = 0;
               TmpTrack.pregap_dv = 0;
               TmpTrack.postgap = 0;
               TmpTrack.index[0] = -1;
               TmpTrack.index[1] = 0;
            }
            active_track = atoi(args[0].c_str());

            if(active_track < self->FirstTrack)
               self->FirstTrack = active_track;
            if(active_track > self->LastTrack)
               self->LastTrack = active_track;

            int format_lookup;
            for(format_lookup = 0; format_lookup < _DI_FORMAT_COUNT; format_lookup++)
            {
               if(!strcasecmp(args[1].c_str(), DI_CUE_Strings[format_lookup]))
               {
                  TmpTrack.DIFormat = format_lookup;
                  break;
               }
            }

            if(format_lookup == _DI_FORMAT_COUNT)
            {
               MDFN_Error(0, "Invalid track format: %s\n", args[1].c_str());
               { ok = false; goto cleanup; }
            }

            if(active_track < 0 || active_track > 99)
            {
               MDFN_Error(0, "Invalid track number: %d\n", active_track);
               { ok = false; goto cleanup; }
            }
         }
         else if(cmdbuf == "INDEX")
         {
            if(active_track >= 0)
            {
               unsigned int m,s,f;

               if(sscanf(args[1].c_str(), "%u:%u:%u", &m, &s, &f) != 3)
               {
                  MDFN_Error(0, "Malformed m:s:f time in \"%s\" directive: %s", cmdbuf.c_str(), args[0].c_str());
                  { ok = false; goto cleanup; }
               }

               if(!strcasecmp(args[0].c_str(), "01") || !strcasecmp(args[0].c_str(), "1"))
                  TmpTrack.index[1] = (m * 60 + s) * 75 + f;
               else if(!strcasecmp(args[0].c_str(), "00") || !strcasecmp(args[0].c_str(), "0"))
                  TmpTrack.index[0] = (m * 60 + s) * 75 + f;
            }
         }
         else if(cmdbuf == "PREGAP")
         {
            if(active_track >= 0)
            {
               unsigned int m,s,f;

               if(sscanf(args[0].c_str(), "%u:%u:%u", &m, &s, &f) != 3)
               {
                  MDFN_Error(0, "Malformed m:s:f time in \"%s\" directive: %s", cmdbuf.c_str(), args[0].c_str());
                  { ok = false; goto cleanup; }
               }

               TmpTrack.pregap = (m * 60 + s) * 75 + f;
            }
         }
         else if(cmdbuf == "POSTGAP")
         {
            if(active_track >= 0)
            {
               unsigned int m,s,f;

               if(sscanf(args[0].c_str(), "%u:%u:%u", &m, &s, &f) != 3)
               {
                  MDFN_Error(0, "Malformed m:s:f time in \"%s\" directive: %s", cmdbuf.c_str(), args[0].c_str());
                  { ok = false; goto cleanup; }
               }      

               TmpTrack.postgap = (m * 60 + s) * 75 + f;
            }
         }
         else if(cmdbuf == "REM")
         {

         }
         else if(cmdbuf == "FLAGS")
         {
            TmpTrack.subq_control &= ~(SUBQ_CTRLF_PRE | SUBQ_CTRLF_DCP | SUBQ_CTRLF_4CH);
            for(unsigned i = 0; i < argcount; i++)
            {
               if(args[i] == "DCP")
               {
                  TmpTrack.subq_control |= SUBQ_CTRLF_DCP;
               }
               else if(args[i] == "4CH")
               {
                  TmpTrack.subq_control |= SUBQ_CTRLF_4CH;
               }
               else if(args[i] == "PRE")
               {
                  TmpTrack.subq_control |= SUBQ_CTRLF_PRE;
               }
               else if(args[i] == "SCMS")
               {
                  // Not implemented, likely pointless.  PROBABLY indicates that the copy bit of the subchannel Q control field is supposed to
                  // alternate between 1 and 0 at 9.375 Hz(four 1, four 0, four 1, four 0, etc.).
               }
               else
               {
                  MDFN_Error(0, "Unknown CUE sheet \"FLAGS\" directive flag \"%s\".\n", args[i].c_str());
                  { ok = false; goto cleanup; }
               }
            }
         }
         else if(cmdbuf == "CDTEXTFILE" || cmdbuf == "CATALOG" || cmdbuf == "ISRC" ||
               cmdbuf == "TITLE" || cmdbuf == "PERFORMER" || cmdbuf == "SONGWRITER")
            log_cb(RETRO_LOG_ERROR, "Unsupported CUE sheet directive: \"%s\".\n", cmdbuf.c_str());
         else
         {
            MDFN_Error(0, "Unknown CUE sheet directive \"%s\".\n", cmdbuf.c_str());
            { ok = false; goto cleanup; }
         }
      } // end of CUE sheet handling
   } // end of fgets() loop

   if(active_track >= 0)
      memcpy(&self->Tracks[active_track], &TmpTrack, sizeof(TmpTrack));

   if(self->FirstTrack > self->LastTrack)
   {
      MDFN_Error(0, "No tracks found!\n");
      { ok = false; goto cleanup; }
   }

   self->NumTracks  = 1 + self->LastTrack - self->FirstTrack;

   for(int x = self->FirstTrack; x < (self->FirstTrack + self->NumTracks); x++)
   {
      if(self->Tracks[x].DIFormat == DI_FORMAT_AUDIO)
         self->Tracks[x].subq_control &= ~SUBQ_CTRLF_DATA;
      else
         self->Tracks[x].subq_control |= SUBQ_CTRLF_DATA;

      if(!IsTOC)	// TOC-format self->disc_type calculation is handled differently.
      {
         switch(self->Tracks[x].DIFormat)
         {
            case DI_FORMAT_MODE2:
            case DI_FORMAT_MODE2_FORM1:
            case DI_FORMAT_MODE2_FORM2:
            case DI_FORMAT_MODE2_RAW:
               self->disc_type = DISC_TYPE_CD_XA;	
               break;
            default:
               break;
         }
      }

      if(IsTOC)
      {
         RunningLBA += self->Tracks[x].pregap;
         self->Tracks[x].LBA = RunningLBA;
         RunningLBA += self->Tracks[x].sectors;
         RunningLBA += self->Tracks[x].postgap;
      }
      else // else handle CUE sheet...
      {
         if(self->Tracks[x].FirstFileInstance) 
         {
            LastIndex = 0;
            FileOffset = 0;
         }

         RunningLBA += self->Tracks[x].pregap;

         self->Tracks[x].pregap_dv = 0;

         if(self->Tracks[x].index[0] != -1)
            self->Tracks[x].pregap_dv = self->Tracks[x].index[1] - self->Tracks[x].index[0];

         FileOffset += self->Tracks[x].pregap_dv * DI_Size_Table[self->Tracks[x].DIFormat];

         RunningLBA += self->Tracks[x].pregap_dv;

         self->Tracks[x].LBA = RunningLBA;

         // Make sure FileOffset this is set before the call to CDAccess_Image_GetSectorCount(self)
         self->Tracks[x].FileOffset = FileOffset;
         self->Tracks[x].sectors = CDAccess_Image_GetSectorCount(self, &self->Tracks[x]);

         if((x + 1) >= (self->FirstTrack + self->NumTracks) || self->Tracks[x+1].FirstFileInstance)
         {

         }
         else
         { 
            // Fix the sector count if we have multiple tracks per one binary image file.
            if(self->Tracks[x + 1].index[0] == -1)
               self->Tracks[x].sectors = self->Tracks[x + 1].index[1] - self->Tracks[x].index[1];
            else
               self->Tracks[x].sectors = self->Tracks[x + 1].index[0] - self->Tracks[x].index[1];	//self->Tracks[x + 1].index - self->Tracks[x].index;
         }

         RunningLBA += self->Tracks[x].sectors;
         RunningLBA += self->Tracks[x].postgap;

         FileOffset += self->Tracks[x].sectors * DI_Size_Table[self->Tracks[x].DIFormat];
      } // end to cue sheet handling
   } // end to track loop

   self->total_sectors = RunningLBA;

   //
   // Load SBI file, if present
   //
   if(!IsTOC)
   {
      std::string sbi_path;
      char sbi_ext[4] = { 's', 'b', 'i', 0 };

      if(file_ext.length() == 4 && file_ext[0] == '.')
      {
         unsigned i;
         for(i = 0; i < 3; i++)
         {
            if(file_ext[1 + i] >= 'A' && file_ext[1 + i] <= 'Z')
               sbi_ext[i] += 'A' - 'a';
         }
      }

      sbi_path = MDFN_EvalFIP(self->base_dir, file_base + std::string(".") + std::string(sbi_ext));

      if (filestream_exists(sbi_path.c_str()))
         CDAccess_Image_LoadSBI(self, sbi_path.c_str());
   }

cleanup:
   stream_close(&fp.base);
   return ok;
}

static void CDAccess_Image_Cleanup(CDAccess_Image *self){
   int32_t track;

   for(track = 0; track < 100; track++)
   {
      CDRFILE_TRACK_INFO *this_track = &self->Tracks[track];

      if(this_track->FirstFileInstance)
      {
         if(self->Tracks[track].AReader)
         {
            delete self->Tracks[track].AReader;
            self->Tracks[track].AReader = NULL;
         }

         if(this_track->fp)
         {
            stream_destroy(this_track->fp);
            this_track->fp = NULL;
         }
      }
   }
}

/* (constructor/destructor handled by factory below) */


/* (constructor/destructor handled by factory below) */


static bool CDAccess_Image_Read_Raw_Sector(CDAccess *base_self, uint8 *buf, int32 lba){
   CDAccess_Image *self = (CDAccess_Image *)base_self;
   int32_t track;
   uint8_t SimuQ[0xC];
   bool TrackFound = false;

   memset(buf + 2352, 0, 96);

   CDAccess_Image_MakeSubPQ(self, lba, buf + 2352);

   subq_deinterleave(buf + 2352, SimuQ);

   for(track = self->FirstTrack; track < (self->FirstTrack + self->NumTracks); track++)
   {
      CDRFILE_TRACK_INFO *ct = &self->Tracks[track];

      if(lba >= (ct->LBA - ct->pregap_dv - ct->pregap) && lba < (ct->LBA + ct->sectors + ct->postgap))
      {
         TrackFound = true;

         // Handle pregap and postgap reading
         if(lba < (ct->LBA - ct->pregap_dv) || lba >= (ct->LBA + ct->sectors))
         {
            memset(buf, 0, 2352);	// Null sector data, per spec
         }
         else
         {
            if(ct->AReader)
            {
               int16 AudioBuf[588 * 2];
               int frames_read = ct->AReader->Read((ct->FileOffset / 4) + (lba - ct->LBA) * 588, AudioBuf, 588);

               ct->LastSamplePos += frames_read;

               if(frames_read < 0 || frames_read > 588)	// This shouldn't happen.
                  frames_read = 0;

               if(frames_read < 588)
                  memset((uint8 *)AudioBuf + frames_read * 2 * sizeof(int16), 0, (588 - frames_read) * 2 * sizeof(int16));

               for(int i = 0; i < 588 * 2; i++)
                  MDFN_en16lsb(buf + i * 2, AudioBuf[i]);
            }
            else	// Binary, woo.
            {
               long SeekPos = ct->FileOffset;
               long LBARelPos = lba - ct->LBA;

               SeekPos += LBARelPos * DI_Size_Table[ct->DIFormat];

               if(ct->SubchannelMode)
                  SeekPos += 96 * (lba - ct->LBA);

               stream_seek(ct->fp, SeekPos, SEEK_SET);

               switch(ct->DIFormat)
               {
                  case DI_FORMAT_AUDIO:
                     stream_read(ct->fp, buf, 2352);

                     if(ct->RawAudioMSBFirst)
                        Endian_A16_Swap(buf, 588 * 2);
                     break;

                  case DI_FORMAT_MODE1:
                     stream_read(ct->fp, buf + 12 + 3 + 1, 2048);
                     encode_mode1_sector(lba + 150, buf);
                     break;

                  case DI_FORMAT_MODE1_RAW:
                  case DI_FORMAT_MODE2_RAW:
                     stream_read(ct->fp, buf, 2352);
                     break;

                  case DI_FORMAT_MODE2:
                     stream_read(ct->fp, buf + 16, 2336);
                     encode_mode2_sector(lba + 150, buf);
                     break;


                     // FIXME: M2F1, M2F2, does sub-header come before or after user data(standards say before, but I wonder
                     // about cdrdao...).
                  case DI_FORMAT_MODE2_FORM1:
                     stream_read(ct->fp, buf + 24, 2048);
                     //encode_mode2_form1_sector(lba + 150, buf);
                     break;

                  case DI_FORMAT_MODE2_FORM2:
                     stream_read(ct->fp, buf + 24, 2324);
                     //encode_mode2_form2_sector(lba + 150, buf);
                     break;

               }

               if(ct->SubchannelMode)
                  stream_read(ct->fp, buf + 2352, 96);
            }
         } // end if audible part of audio track read.
         break;
      } // End if LBA is in range
   } // end track search loop

   if(!TrackFound)
   {
      MDFN_Error(0, "Could not find track for sector %u!", lba);
      return false;
   }

   return true;
}

// Note: this function makes use of the current contents(as in |=) in SubPWBuf.
static void CDAccess_Image_MakeSubPQ(CDAccess_Image *self, int32 lba, uint8 *SubPWBuf){
   unsigned i;
   uint8_t buf[0xC], adr, control;
   int32_t track;
   uint32_t lba_relative;
   uint32_t ma, sa, fa;
   uint32_t m, s, f;
   uint8_t pause_or = 0x00;
   bool track_found = false;

   for(track = self->FirstTrack; track < (self->FirstTrack + self->NumTracks); track++)
   {
      if(lba >= (self->Tracks[track].LBA - self->Tracks[track].pregap_dv - self->Tracks[track].pregap) 
            && lba < (self->Tracks[track].LBA + self->Tracks[track].sectors + self->Tracks[track].postgap))
      {
         track_found = true;
         break;
      }
   }

   if(!track_found)
      track = self->FirstTrack;

   lba_relative = abs((int32)lba - self->Tracks[track].LBA);

   f            = (lba_relative % 75);
   s            = ((lba_relative / 75) % 60);
   m            = (lba_relative / 75 / 60);

   fa           = (lba + 150) % 75;
   sa           = ((lba + 150) / 75) % 60;
   ma           = ((lba + 150) / 75 / 60);

   adr          = 0x1; // Q channel data encodes position
   control      = self->Tracks[track].subq_control;

   // Handle pause(D7 of interleaved subchannel byte) bit, should be set to 1 when in pregap or postgap.
   if((lba < self->Tracks[track].LBA) || (lba >= self->Tracks[track].LBA + self->Tracks[track].sectors))
      pause_or = 0x80;

   // Handle pregap between audio->data track
   {
      int32_t pg_offset = (int32)lba - self->Tracks[track].LBA;

      // If we're more than 2 seconds(150 sectors) from the real "start" of the track/INDEX 01, and the track is a data track,
      // and the preceding track is an audio track, encode it as audio(by taking the SubQ control field from the preceding track).
      //
      // TODO: Look into how we're supposed to handle subq control field in the four combinations of track types(data/audio).
      //
      if(pg_offset < -150)
      {
         if((self->Tracks[track].subq_control & SUBQ_CTRLF_DATA) && (self->FirstTrack < track) && !(self->Tracks[track - 1].subq_control & SUBQ_CTRLF_DATA))
            control = self->Tracks[track - 1].subq_control;
      }
   }

   memset(buf, 0, 0xC);
   buf[0] = (adr << 0) | (control << 4);
   buf[1] = U8_to_BCD(track);

   if(lba < self->Tracks[track].LBA) // Index is 00 in pregap
      buf[2] = U8_to_BCD(0x00);
   else
      buf[2] = U8_to_BCD(0x01);

   /* Track relative MSF address */
   buf[3] = U8_to_BCD(m);
   buf[4] = U8_to_BCD(s);
   buf[5] = U8_to_BCD(f);
   buf[6] = 0;
   /* Absolute MSF address */
   buf[7] = U8_to_BCD(ma);
   buf[8] = U8_to_BCD(sa);
   buf[9] = U8_to_BCD(fa);

   subq_generate_checksum(buf);

   if(!self->SubQReplaceMap.empty())
   {
      std::map<uint32, cpp11_array_doodad>::const_iterator it = self->SubQReplaceMap.find(LBA_to_ABA(lba));

      if(it != self->SubQReplaceMap.end())
         memcpy(buf, it->second.data, 12);
   }

   for (i = 0; i < 96; i++)
      SubPWBuf[i] |= (((buf[i >> 3] >> (7 - (i & 0x7))) & 1) ? 0x40 : 0x00) | pause_or;
}

static bool CDAccess_Image_Read_Raw_PW(CDAccess *base_self, uint8_t *buf, int32_t lba){
   CDAccess_Image *self = (CDAccess_Image *)base_self;
   memset(buf, 0, 96);
   CDAccess_Image_MakeSubPQ(self, lba, buf);
   return true;
}

static bool CDAccess_Image_Read_TOC(CDAccess *base_self, TOC *toc){
   CDAccess_Image *self = (CDAccess_Image *)base_self;
   unsigned i;

   TOC_Clear(toc);

   toc->first_track = self->FirstTrack;
   toc->last_track = self->FirstTrack + self->NumTracks - 1;
   toc->disc_type = self->disc_type;

   for(i = toc->first_track; i <= toc->last_track; i++)
   {
      toc->tracks[i].lba = self->Tracks[i].LBA;
      toc->tracks[i].adr = ADR_CURPOS;
      toc->tracks[i].control = self->Tracks[i].subq_control;
   }

   toc->tracks[100].lba = self->total_sectors;
   toc->tracks[100].adr = ADR_CURPOS;
   toc->tracks[100].control = toc->tracks[toc->last_track].control & 0x4;

   // Convenience leadout track duplication.
   if(toc->last_track < 99)
      toc->tracks[toc->last_track + 1] = toc->tracks[100];

   return true;
}

static void CDAccess_Image_Eject(CDAccess *base_self, bool eject_status){
   CDAccess_Image *self = (CDAccess_Image *)base_self;

}

/* ------------------------------------------------------------------
 * destroy and factory.
 * ------------------------------------------------------------------ */

static void CDAccess_Image_destroy(CDAccess *base_self)
{
   CDAccess_Image *self = (CDAccess_Image *)base_self;
   CDAccess_Image_Cleanup(self);
   /* Manually call destructors for the C++ members embedded in self. */
   self->SubQReplaceMap.~map();
   self->base_dir.~basic_string();
   free(self);
}

extern "C" CDAccess *CDAccess_Image_New(bool *success, const char *path,
      bool image_memcache)
{
   CDAccess_Image *self = (CDAccess_Image *)calloc(1, sizeof(*self));
   if (!self)
   {
      *success = false;
      return NULL;
   }

   /* Placement-new the C++ members so their destructors can run later. */
   new (&self->SubQReplaceMap) std::map<uint32, cpp11_array_doodad>();
   new (&self->base_dir) std::string();

   /* Vtable */
   self->base.Read_Raw_Sector = CDAccess_Image_Read_Raw_Sector;
   self->base.Read_Raw_PW     = CDAccess_Image_Read_Raw_PW;
   self->base.Read_TOC        = CDAccess_Image_Read_TOC;
   self->base.Eject           = CDAccess_Image_Eject;
   self->base.destroy         = CDAccess_Image_destroy;

   self->NumTracks     = 0;
   self->FirstTrack    = 0;
   self->LastTrack     = 0;
   self->total_sectors = 0;
   /* Tracks already zeroed by calloc; that matches memset() in the
    * old constructor. */

   if (!CDAccess_Image_ImageOpen(self, path, image_memcache))
      *success = false;

   return &self->base;
}
