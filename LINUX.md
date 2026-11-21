# Running Personal Gallery on Linux (Proton / Wine)

**Status: experimental. The decoder was tested natively on Linux; the full plugin has not yet been run inside
Skyrim under Proton.** Please report what you find (see "What to send back").

The mod is a Windows SKSE plugin, so on Linux it runs inside Proton like every other SKSE mod. Nothing here
makes a native Linux build; it removes the parts that needed Windows itself.

## What was the problem

| Part | Windows API | Under Proton |
|---|---|---|
| Video + video sound decoding | Media Foundation | only partly implemented, codec support unreliable |
| WebP / odd still images | WIC | Wine's WIC has fewer codecs than Windows |
| Menu font | `C:\Windows\Fonts\georgia.ttf` etc. | those files don't exist in a Proton prefix |
| Video sound output | XAudio2 2.9 | provided by Proton (FAudio), expected to work |
| Menu, textures, filters | D3D11, DirectXTex, Dear ImGui | run on DXVK, expected to work |

## What this change does

* **`src/VideoFFmpeg.cpp`** is a new decoder backend on FFmpeg (libavformat/avcodec/swscale/swresample), linked
  *into the plugin DLL* so it needs nothing from the OS. It implements the same `Video::Decoder` interface, so
  `Player.cpp` is untouched. Output is identical in shape: top-down BGRA frames, optional down-scaling to the
  canvas, 16-bit mono/stereo audio.
* **`Video.cpp`** chooses the backend:
  * under Wine/Proton (detected via `wine_get_version` in ntdll): FFmpeg first, Media Foundation as fallback;
  * on real Windows: Media Foundation first (hardware decode, as before), FFmpeg as fallback for what it can't
    open (VP9/AV1/HEVC without Store extensions, MKV, WebM with Opus/Vorbis, `.mpg`, `.ogv`, `.flv`, `.3gp`).
  * override with the environment variable `PG_VIDEO_BACKEND=ffmpeg` or `=windows`
    (Steam launch options: `PG_VIDEO_BACKEND=ffmpeg %command%`).
* **`Pictures.cpp`**: if WIC can't read a still image (WebP etc.), FFmpeg does.
* **`UI.cpp`**: the menu font falls back to Proton's Liberation fonts, then the host's fonts through Wine's `Z:`
  drive (DejaVu / Liberation / Noto in the usual Arch, Fedora and Debian locations), then any `.ttf` in the
  prefix, then ImGui's built-in font. Previously it went straight to ImGui's tiny default.
* Build: CMake option `PG_FFMPEG` (default ON); `vcpkg.json` gains `ffmpeg` (core libraries only, no GPL or
  non-free parts, so LGPL code inside this GPL-3 plugin). With `-DPG_FFMPEG=OFF` you get the original build.

## Using it (players)

1. Install through Fluorine / MO2 as normal, with SKSE, Address Library, PapyrusUtil, SkyUI.
2. Start Skyrim through SKSE under a recent **Proton-GE or Proton Experimental** prefix.
3. Pictures go in `Data/PersonalGallery/Pictures`, Home opens the gallery.
4. Under a fresh prefix you may need the usual prefix pieces other SKSE mods need (VC++ runtimes via protontricks).

## Building the plugin (developers)

The output is still a Windows DLL, so you still need a Windows toolchain. Options:

* **Windows / Visual Studio**: unchanged, run `build.bat` (the triplet `x64-windows-static-md` links FFmpeg statically).
* **Linux host**: cross-compile with clang-cl + the Windows SDK from `xwin`, CMake, Ninja and vcpkg
  (`VCPKG_TARGET_TRIPLET=x64-windows-static-md`, `VCPKG_CHAINLOAD_TOOLCHAIN_FILE` pointing at a clang-cl toolchain).
  This route is **untested** for this project (CommonLibSSE-NG itself is normally built with MSVC).

## What was tested

Native Linux, FFmpeg 6.1, with the real `VideoFFmpeg.cpp`, built with `-Wall -Wextra` and also under
AddressSanitizer + UBSan (no findings, no leaks):

* H.264/AAC MP4 (1280x720, 30 fps): size, 30 fps, 3.0 s duration, exactly 90 frames, strictly increasing
  timestamps, first frame at t=0, rewind, 3 consecutive loops of 90 frames, frame-accurate seek (1.5 s lands at
  1.500), `skipBefore` skips conversion for early frames, down-scaling to the canvas width (854x480 for a 4:3
  canvas at 640 px).
* VP9 WebM (48 frames), animated GIF (20 frames, 2.0 s, rewind), WebP and PNG stills and their sizes.
* Audio: stereo 44.1 kHz, a 440 Hz sine measures 440 Hz and 3.0 s; a 5.1 AAC track folds to stereo with the
  right pitch; no audio track returns "none"; stopping via the run flag aborts.
* Colour: BGRA channel order verified; BT.709 vs BT.601 matrix chosen from the stream (HD without metadata is
  treated as BT.709); decoding speed ~260 fps for 720p on one test machine.
* The Windows-facing files `Video.cpp` and `VideoFFmpeg.cpp` pass a MinGW `-fsyntax-only` check with and
  without `PG_WITH_FFMPEG`, including the 16-bit `wchar_t` path conversion.

## Not tested / known gaps

* `Pictures.cpp` and `UI.cpp` could not be compiled here (they need CommonLibSSE-NG, DirectXTex, Dear ImGui);
  those edits are small and were reviewed by eye. Expect to fix a typo or two on first build.
* Nothing was run inside Skyrim. The unknowns are DXVK + the Present hook, ImGui input under Wayland/niri
  (try Gamescope or XWayland if the Home key / mouse capture misbehave), and XAudio2 under Proton.
* Rotated phone videos (rotation metadata) are not auto-rotated; neither was the Media Foundation path.
* Software decoding is used on the FFmpeg path; a 4K HEVC clip will be heavier than with hardware decode on Windows.

## What to send back

Enable the dev log by creating an empty `Data/PersonalGallery/dev.txt`, play a video, and send
`Data/PersonalGallery/dev_log.txt` plus `skse64.log`. Lines tagged `video` say which decoder opened the file.
