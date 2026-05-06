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

#ifndef _GENERAL_C_H
#define _GENERAL_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C-callable counterparts to the std::string entry points in
 * general.h.  These let plain-C TUs (cdrom backends mid-conversion,
 * etc.) drive the same path-manipulation primitives via char
 * buffers without pulling <string> in.
 *
 * Buffer ownership in all cases is the caller's; on overflow the
 * output is truncated and NUL-terminated (no error reporting).
 *
 * The C++ overloads in general.h forward to these. */

/* MDFN_GetFilePathComponents_c: split file_path into directory,
 * file base name, and extension.  Any of dir_out / base_out /
 * ext_out may be NULL to skip that component.  *_outlen specifies
 * each output buffer's capacity in bytes.  ext_out includes the
 * leading '.' (matches the std::string overload's behaviour). */
void MDFN_GetFilePathComponents_c(const char *file_path,
      char *dir_out,  size_t dir_outlen,
      char *base_out, size_t base_outlen,
      char *ext_out,  size_t ext_outlen);

/* MDFN_EvalFIP_c: resolve rel_path relative to dir_path into
 * out / outlen.  If rel_path is absolute it's copied verbatim;
 * otherwise dir_path is prepended with the platform separator. */
void MDFN_EvalFIP_c(const char *dir_path, const char *rel_path,
      char *out, size_t outlen);

/* MDFN_trim_c: in-place trim leading and trailing whitespace
 * (' ', '\r', '\n', '\t', '\v') from str. */
void MDFN_trim_c(char *str);

/* MDFN_strtoupper_c: in-place lowercase->uppercase ASCII conversion.
 * Counterpart to misc.h's MDFN_strtoupper(std::string&). */
void MDFN_strtoupper_c(char *str);

#ifdef __cplusplus
}
#endif

#endif
