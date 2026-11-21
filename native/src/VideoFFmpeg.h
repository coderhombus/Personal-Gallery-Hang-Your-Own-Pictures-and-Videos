// Personal Gallery - a Skyrim Special Edition mod (SKSE plugin PersonalGalleryNative.dll)
// Copyright (C) 2026 the Personal Gallery author
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program (see
// LICENSE.txt). If not, see <https://www.gnu.org/licenses/>.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Video.h"

// FFmpeg (libavformat / libavcodec / libswscale / libswresample) backend for Video.h.
//
// Windows Media Foundation is not available in a usable form under Wine / Proton (Linux), so this backend
// does the same job with libraries that are linked into the plugin itself and need nothing from the OS.
// It is also used on Windows as a fallback for files Media Foundation can't open (VP9, AV1, HEVC without
// the Store extensions, MKV, WebM ...) and for still images WIC can't read (WebP ...).
//
// Only compiled in when PG_WITH_FFMPEG is defined (see CMakeLists.txt, option PG_FFMPEG).
namespace Video::FF
{
	// Video (or animated GIF) decoder, output BGRA top-down, same contract as Video::Open.
	std::unique_ptr<Decoder> Open(const std::wstring& a_path, float a_aspect, int a_maxWidth);

	// Whole soundtrack as 16-bit PCM, mono or stereo (more channels are folded down). False: no audio stream
	// or it can't be decoded. Stops early (returns false) when a_run turns false.
	bool ReadAudio(const std::wstring& a_path, const std::atomic<bool>* a_run, Audio& a_out);

	// First frame of an image file (PNG, JPEG, WebP, BMP, TIFF, GIF ...) as tightly packed RGBA, top-down.
	bool LoadStill(const std::wstring& a_path, int& a_w, int& a_h, std::vector<std::uint8_t>& a_rgba);

	// Pixel size of an image file without decoding it.
	bool StillSize(const std::wstring& a_path, int& a_w, int& a_h);
}
