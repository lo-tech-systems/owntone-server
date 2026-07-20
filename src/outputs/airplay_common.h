/*
 * Copyright (C) 2026 James Pearce
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

#ifndef __AIRPLAY_COMMON_H__
#define __AIRPLAY_COMMON_H__

// Which buffered (type 103) encode profile a master session or a pending
// session decision selects. NONE means the realtime ALAC transport, i.e. not
// buffered at all.
enum airplay_buffered_kind
{
  AIRPLAY_BUFFERED_KIND_NONE,
  AIRPLAY_BUFFERED_KIND_AAC_STEREO,       // bufferStream format 23
  AIRPLAY_BUFFERED_KIND_AAC44_STEREO,     // bufferStream format 22 (AAC-LC 44.1kHz)
  AIRPLAY_BUFFERED_KIND_ALAC24,           // bufferStream format 21
  // Both of the below stream bufferStream format 39 (AAC-LC 48kHz 5.1); they
  // differ only in how the stereo source is placed into the six channels.
  AIRPLAY_BUFFERED_KIND_SURROUND_STEREO,  // static FL/FR/LFE pan, silent C/rear
  AIRPLAY_BUFFERED_KIND_SURROUND_UPMIX,   // decode-steered upmix to all 6 channels
};

#endif /* !__AIRPLAY_COMMON_H__ */
