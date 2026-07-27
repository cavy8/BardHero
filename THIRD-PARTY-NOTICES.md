# Third-party notices

SkyHero vendors the following libraries. Formats are implemented from the CC0
GuitarGame_ChartFormats specification; YARG was studied read-only and no code
from it is included (see docs/specs/2026-07-18-rhythm-engine-design.md §12).

## miniaudio v0.11.21
MIT-0 / public domain dual license. https://miniaud.io
Vendored unmodified as extern/miniaudio/miniaudio.h.

## stb_vorbis v1.22
Public domain / MIT dual license. https://github.com/nothings/stb
Vendored unmodified as extern/stb/stb_vorbis.c.

## ma_libopus (miniaudio libopus extra, v0.11.21)
MIT-0 / public domain dual license (same terms as miniaudio).
https://github.com/mackron/miniaudio
Vendored as extern/miniaudio/ma_libopus.h, fetched from
https://raw.githubusercontent.com/mackron/miniaudio/0.11.21/extras/miniaudio_libopus.h
This 0.11.21 header exposes the libopus decoder as a data source only; the
ma_decoding_backend_vtable glue that registers it with the resource manager is
hand-written in src/MiniaudioImpl.cpp per miniaudio's custom_decoders example.

## signalsmith-stretch v1.3.2
MIT license. Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd.
https://github.com/Signalsmith-Audio/signalsmith-stretch
Vendored unmodified as extern/signalsmith/signalsmith-stretch.h, fetched from
https://raw.githubusercontent.com/Signalsmith-Audio/signalsmith-stretch/main/signalsmith-stretch.h
at commit 57b93f4e9206a089a45387eaa39bdc9f310d3308 (2025-05-25). The version
is the header's own `version[3] = {1, 3, 2}`. Pitch-preserving time stretch
(phase vocoder) for practice-mode playback speed - slowed playback keeps its
pitch instead of detuning like varispeed would.

## signalsmith-linear (stft + fft)
MIT license. Copyright (c) 2025 Signalsmith Audio.
https://github.com/Signalsmith-Audio/linear
Vendored unmodified as extern/signalsmith/signalsmith-linear/stft.h and
extern/signalsmith/signalsmith-linear/fft.h, fetched from
https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/stft.h and
https://raw.githubusercontent.com/Signalsmith-Audio/linear/main/fft.h
at commit 7f53cdd1ccd52b409dacf2af24e7ff838c5580cd (2026-06-30).
This is signalsmith-stretch's only dependency: it includes
"signalsmith-linear/stft.h", which in turn includes "./fft.h". Only those two
files are vendored - fft.h's "./platform/fft-*.h" includes are opt-in behind
SIGNALSMITH_USE_PFFFT / _PFFFT_DOUBLE / _ACCELERATE / _IPP, none of which we
define, so those platform backends are never reached and were not fetched.
Neither upstream repo puts a copyright banner in the headers themselves; the
license text lives in each repo's LICENSE.txt.

## libopus
BSD 3-Clause license. https://opus-codec.org
Linked (not vendored) via vcpkg (opus). The Opus codec, Xiph.Org Foundation.

## opusfile (libopusfile)
BSD 3-Clause license. https://opus-codec.org
Linked (not vendored) via vcpkg (opusfile). Ogg Opus stream decoding,
Xiph.Org Foundation. Depends on libogg (BSD 3-Clause), also linked via vcpkg.

## libopusenc
BSD 3-Clause license. https://opus-codec.org
Linked (not vendored) via vcpkg (libopusenc). Used only to create the local
Ogg Opus cache for a separately installed BA Bard Songs copy.
