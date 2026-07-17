#ifndef __AIRPLAY_COMMON_H__
#define __AIRPLAY_COMMON_H__

// Which buffered (type 103) encode profile a master session or a pending
// session decision selects. NONE means the realtime ALAC transport, i.e. not
// buffered at all.
enum airplay_buffered_kind
{
  AIRPLAY_BUFFERED_KIND_NONE,
  AIRPLAY_BUFFERED_KIND_AAC_STEREO,       // bufferStream format 23
  AIRPLAY_BUFFERED_KIND_ALAC24,           // bufferStream format 21
  // Both of the below stream bufferStream format 39 (AAC-LC 48kHz 5.1); they
  // differ only in how the stereo source is placed into the six channels.
  AIRPLAY_BUFFERED_KIND_SURROUND_STEREO,  // static FL/FR/LFE pan, silent C/rear
  AIRPLAY_BUFFERED_KIND_SURROUND_UPMIX,   // decode-steered upmix to all 6 channels
};

#endif /* !__AIRPLAY_COMMON_H__ */
