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
#include <stddef.h>
#include <string.h>

#include <file/file_path.h>

#include <string>

#include "general.h"
#include "general_c.h"

/* ------------------------------------------------------------------
 * Plain-C primitives.  The std::string overloads at the bottom of
 * this file forward to these.
 * ------------------------------------------------------------------ */

static void copy_truncate(char *out, size_t outlen, const char *src,
      size_t srclen)
{
   if (!out || outlen == 0)
      return;
   if (srclen >= outlen)
      srclen = outlen - 1;
   memcpy(out, src, srclen);
   out[srclen] = 0;
}

extern "C" void MDFN_GetFilePathComponents_c(const char *file_path,
      char *dir_out,  size_t dir_outlen,
      char *base_out, size_t base_outlen,
      char *ext_out,  size_t ext_outlen)
{
   const char *p_slash;
   const char *p_dot;
   const char *file_name;
   size_t      file_path_len = strlen(file_path);
   size_t      dir_len, base_len, ext_len;

#ifdef _WIN32
   {
      const char *p_back = strrchr(file_path, '\\');
      const char *p_fwd  = strrchr(file_path, '/');
      p_slash = (p_back && (!p_fwd || p_fwd < p_back)) ? p_back : p_fwd;
   }
#else
   p_slash = strrchr(file_path, '/');
#endif

   if (!p_slash)
   {
      copy_truncate(dir_out, dir_outlen, ".", 1);
      file_name = file_path;
      dir_len   = 1;
      (void)dir_len;
   }
   else
   {
      dir_len   = (size_t)(p_slash - file_path);
      copy_truncate(dir_out, dir_outlen, file_path, dir_len);
      file_name = p_slash + 1;
   }

   p_dot = strrchr(file_name, '.');

   if (p_dot)
   {
      base_len = (size_t)(p_dot - file_name);
      ext_len  = file_path_len - (size_t)(p_dot - file_path);
      copy_truncate(base_out, base_outlen, file_name, base_len);
      copy_truncate(ext_out,  ext_outlen,  p_dot,     ext_len);
   }
   else
   {
      base_len = strlen(file_name);
      copy_truncate(base_out, base_outlen, file_name, base_len);
      copy_truncate(ext_out,  ext_outlen,  "",        0);
   }
}

extern "C" void MDFN_EvalFIP_c(const char *dir_path, const char *rel_path,
      char *out, size_t outlen)
{
#ifdef _WIN32
   const char slash = '\\';
#else
   const char slash = '/';
#endif
   size_t dlen, rlen, total;

   if (!out || outlen == 0)
      return;

   if (path_is_absolute(rel_path))
   {
      copy_truncate(out, outlen, rel_path, strlen(rel_path));
      return;
   }

   dlen = strlen(dir_path);
   rlen = strlen(rel_path);
   total = dlen + 1 + rlen;
   if (total >= outlen)
      total = outlen - 1;

   if (dlen >= outlen)
      dlen = outlen - 1;
   memcpy(out, dir_path, dlen);
   {
      size_t pos = dlen;
      if (pos + 1 < outlen)
         out[pos++] = slash;
      if (pos < outlen)
      {
         size_t can = outlen - 1 - pos;
         if (rlen > can)
            rlen = can;
         memcpy(out + pos, rel_path, rlen);
         pos += rlen;
      }
      if (pos >= outlen)
         pos = outlen - 1;
      out[pos] = 0;
   }
}

static int is_trim_ws(char c)
{
   return (c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == 0x0b);
}

extern "C" void MDFN_trim_c(char *str)
{
   size_t len, di, si;
   int    seen_nonws;

   if (!str)
      return;

   /* rtrim */
   len = strlen(str);
   while (len > 0 && is_trim_ws(str[len - 1]))
   {
      len--;
      str[len] = 0;
   }

   /* ltrim - shift left */
   di = si = 0;
   seen_nonws = 0;
   while (str[si])
   {
      if (!seen_nonws && is_trim_ws(str[si]))
      {
         si++;
         continue;
      }
      seen_nonws = 1;
      str[di++] = str[si++];
   }
   str[di] = 0;
}

extern "C" void MDFN_strtoupper_c(char *str)
{
   if (!str)
      return;
   for (; *str; str++)
   {
      if (*str >= 'a' && *str <= 'z')
         *str = (char)(*str - 'a' + 'A');
   }
}

/* ------------------------------------------------------------------
 * std::string overloads - call the _c primitives via temp buffers.
 *
 * These are the legacy entry points still used by libretro.cpp and
 * the four CDAccess_*.cpp backends; they'll go away when the last
 * std::string-only caller converts.  Until then they do an extra
 * char-buffer round-trip - cost is negligible (one alloc + copy
 * per call, only invoked at disc-image-open time).
 * ------------------------------------------------------------------ */

#define MDFN_PATH_BUF 4096

void MDFN_GetFilePathComponents(const std::string &file_path,
      std::string *dir_path_out, std::string *file_base_out,
      std::string *file_ext_out)
{
   char dir[MDFN_PATH_BUF];
   char base[MDFN_PATH_BUF];
   char ext[MDFN_PATH_BUF];

   MDFN_GetFilePathComponents_c(file_path.c_str(),
         dir_path_out  ? dir  : NULL, dir_path_out  ? sizeof(dir)  : 0,
         file_base_out ? base : NULL, file_base_out ? sizeof(base) : 0,
         file_ext_out  ? ext  : NULL, file_ext_out  ? sizeof(ext)  : 0);

   if (dir_path_out)  *dir_path_out  = dir;
   if (file_base_out) *file_base_out = base;
   if (file_ext_out)  *file_ext_out  = ext;
}

std::string MDFN_EvalFIP(const std::string &dir_path,
      const std::string &rel_path)
{
   char buf[MDFN_PATH_BUF];
   MDFN_EvalFIP_c(dir_path.c_str(), rel_path.c_str(), buf, sizeof(buf));
   return std::string(buf);
}

void MDFN_trim(std::string &str)
{
   /* The string ops are simple enough to keep an in-place
    * implementation here rather than round-trip through a buffer. */
   size_t i, len = str.length();

   while (len > 0
         && (str[len - 1] == ' '  || str[len - 1] == '\r'
          || str[len - 1] == '\n' || str[len - 1] == '\t'
          || str[len - 1] == 0x0b))
      len--;
   str.resize(len);

   for (i = 0; i < str.length(); i++)
   {
      char c = str[i];
      if (c != ' ' && c != '\r' && c != '\n' && c != '\t' && c != 0x0b)
         break;
   }
   if (i)
      str.erase(0, i);
}
