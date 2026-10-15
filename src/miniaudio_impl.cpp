// miniaudio_impl.cpp — the ONLY translation unit that compiles the miniaudio
// implementation (defining MINIAUDIO_IMPLEMENTATION in more than one TU
// causes duplicate-symbol link errors).
//
// Decoder enables (task §6):
//   * WAV / FLAC / MP3 are compiled into miniaudio.h by default
//     (MA_HAS_WAV miniaudio.h:61935, FLAC :62370, MP3 :62664; only the
//     MA_NO_WAV / MA_NO_FLAC / MA_NO_MP3 switches remove them — none are
//     defined in premake5.lua).
//   * OGG Vorbis needs stb_vorbis compiled in BEFORE the miniaudio
//     implementation so that MA_HAS_VORBIS (miniaudio.h:65330-65331) sees
//     STB_VORBIS_INCLUDE_STB_VORBIS_H (defined by stb_vorbis.c:74). The
//     decoder is then picked up by extension trial-and-error
//     (miniaudio.h:66916-66918).
//
// The Freeverb reverb node (extras/nodes/ma_reverb_node) implementation is
// also compiled HERE — the single-TU rule extends to the extras code, so
// nothing under external/miniaudio is ever compiled separately.

// Canonical miniaudio + stb_vorbis two-pass include order (verified against the
// guard structure in extras/stb_vorbis.c):
//
//   Pass 1 (HEADER_ONLY): declares the stb_vorbis API and defines
//   STB_VORBIS_INCLUDE_STB_VORBIS_H (stb_vorbis.c:75) — which is exactly the
//   macro the miniaudio implementation tests for MA_HAS_VORBIS
//   (miniaudio.h:65330-65331). The implementation section (:420-5541) is
//   skipped, so its short macros `L`/`C`/`R` (stb_vorbis.c:5131-5133) are NOT
//   yet defined.
//
//   MINIAUDIO_IMPLEMENTATION: miniaudio's Windows backend includes the Windows
//   SDK headers — those parse cleanly because the L/C/R macros do not exist
//   yet (a single full-include-first order corrupts winnt.h bitfields such as
//   `DWORD Flag : 2` in IMAGE_ARM_RUNTIME_FUNCTION_ENTRY: the preprocessor
//   rewrites `R : 1; L : 1; C : 1;` into parenthesized channel masks and the
//   SDK headers explode with C2059/C2334 cascades).
//
//   Pass 2 (full): the header guard is already set, so only the implementation
//   section compiles — AFTER windows.h has been pulled in. Its definitions
//   satisfy the stb_vorbis calls emitted by the miniaudio decoder at link time.
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "extras/stb_vorbis.c"

// Reverb node (VERBLIB_IMPLEMENTATION is defined inside the .c).
#include "extras/nodes/ma_reverb_node/ma_reverb_node.c"
