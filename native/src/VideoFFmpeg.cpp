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

#include "VideoFFmpeg.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <tuple>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#ifdef PG_FF_LOG
#	include "Dev.h"
#	define FFLOG(...) Dev::Log("video", __VA_ARGS__)
#else
#	define FFLOG(...) ((void)0)
#endif

namespace Video::FF
{
	namespace
	{
		// Paths reach us as wstring (UTF-16 on Windows/Wine). FFmpeg wants UTF-8, and on Windows it turns that
		// back into a wide path itself, so non-ASCII file names work.
		std::string ToUtf8(const std::wstring& w)
		{
			std::string out;
			out.reserve(w.size());
			for (size_t i = 0; i < w.size(); ++i) {
				std::uint32_t c = static_cast<std::uint32_t>(w[i]);
				if constexpr (sizeof(wchar_t) == 2) {
					if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) {
						const std::uint32_t lo = static_cast<std::uint32_t>(w[i + 1]);
						if (lo >= 0xDC00 && lo < 0xE000) {
							c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
							++i;
						}
					}
				}
				if (c < 0x80) {
					out += static_cast<char>(c);
				} else if (c < 0x800) {
					out += static_cast<char>(0xC0 | (c >> 6));
					out += static_cast<char>(0x80 | (c & 0x3F));
				} else if (c < 0x10000) {
					out += static_cast<char>(0xE0 | (c >> 12));
					out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (c & 0x3F));
				} else {
					out += static_cast<char>(0xF0 | (c >> 18));
					out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
					out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (c & 0x3F));
				}
			}
			return out;
		}

		[[maybe_unused]] std::string Err(int a_code)
		{
			char buf[AV_ERROR_MAX_STRING_SIZE]{};
			av_strerror(a_code, buf, sizeof(buf));
			return buf;
		}

		// ---- small RAII wrappers ----------------------------------------------------------

		struct FormatDeleter
		{
			void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
		};
		struct CodecDeleter
		{
			void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
		};
		struct FrameDeleter
		{
			void operator()(AVFrame* p) const { av_frame_free(&p); }
		};
		struct PacketDeleter
		{
			void operator()(AVPacket* p) const { av_packet_free(&p); }
		};
		struct SwsDeleter
		{
			void operator()(SwsContext* p) const { sws_freeContext(p); }
		};
		struct SwrDeleter
		{
			void operator()(SwrContext* p) const { swr_free(&p); }
		};

		using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
		using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
		using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
		using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
		using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
		using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

		// A stream's decoder, opened. Threads: a few, not all - the game is running too.
		CodecPtr OpenCodec(AVStream* a_stream, bool a_threads)
		{
			const AVCodec* codec = avcodec_find_decoder(a_stream->codecpar->codec_id);
			if (!codec) {
				FFLOG("no decoder for codec id {}", static_cast<int>(a_stream->codecpar->codec_id));
				return nullptr;
			}
			CodecPtr ctx(avcodec_alloc_context3(codec));
			if (!ctx || avcodec_parameters_to_context(ctx.get(), a_stream->codecpar) < 0) {
				return nullptr;
			}
			ctx->pkt_timebase = a_stream->time_base;
			if (a_threads) {
				const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
				ctx->thread_count = static_cast<int>(std::min(4u, hw > 2 ? hw / 2 : 1u));
				ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
			}
			if (const int r = avcodec_open2(ctx.get(), codec, nullptr); r < 0) {
				FFLOG("avcodec_open2 failed: {}", Err(r));
				return nullptr;
			}
			return ctx;
		}

		FormatPtr OpenFormat(const std::wstring& a_path)
		{
			static const bool quiet = [] {  // FFmpeg's own chatter would otherwise go to stderr (Proton's console)
				av_log_set_level(AV_LOG_ERROR);
				return true;
			}();
			(void)quiet;
			AVFormatContext* raw = nullptr;
			const auto       utf8 = ToUtf8(a_path);
			if (const int r = avformat_open_input(&raw, utf8.c_str(), nullptr, nullptr); r < 0) {
				FFLOG("avformat_open_input failed: {}", Err(r));
				return nullptr;
			}
			FormatPtr fmt(raw);
			if (const int r = avformat_find_stream_info(fmt.get(), nullptr); r < 0) {
				FFLOG("avformat_find_stream_info failed: {}", Err(r));
				return nullptr;
			}
			return fmt;
		}

		// Cover art in an MP3 / M4A shows up as a "video" stream of one picture: not what we want.
		int FindVideoStream(AVFormatContext* a_fmt)
		{
			int best = -1;
			for (unsigned i = 0; i < a_fmt->nb_streams; ++i) {
				const AVStream* s = a_fmt->streams[i];
				if (s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || (s->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
					continue;
				}
				best = static_cast<int>(i);
				break;
			}
			return best;
		}

		// swscale assumes BT.601 / limited range for YUV unless told otherwise, which tints HD video. Use what the
		// stream says, and when it says nothing the usual guess: HD is BT.709, SD is BT.601.
		void SetColourDetails(SwsContext* a_sws, const AVFrame* a_frame)
		{
			int cs = SWS_CS_DEFAULT;
			switch (a_frame->colorspace) {
			case AVCOL_SPC_BT709:
				cs = SWS_CS_ITU709;
				break;
			case AVCOL_SPC_BT470BG:
			case AVCOL_SPC_SMPTE170M:
				cs = SWS_CS_ITU601;
				break;
			case AVCOL_SPC_SMPTE240M:
				cs = SWS_CS_SMPTE240M;
				break;
			case AVCOL_SPC_BT2020_NCL:
			case AVCOL_SPC_BT2020_CL:
				cs = SWS_CS_BT2020;
				break;
			default:
				cs = a_frame->height >= 720 || a_frame->width > 1024 ? SWS_CS_ITU709 : SWS_CS_ITU601;
				break;
			}
			const int  fullRange = a_frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
			const int* table = sws_getCoefficients(cs);
			sws_setColorspaceDetails(a_sws, table, fullRange, table, 1, 0, 1 << 16, 1 << 16);
		}

		// ---- video ----

		class FFDecoder : public Decoder
		{
		public:
			bool Init(const std::wstring& a_path, float a_aspect, int a_maxWidth)
			{
				path = a_path;
				aspect = a_aspect;
				maxWidth = a_maxWidth;
				return Open();
			}

			bool Next(Frame& a_out, double a_skipBefore) override
			{
				for (;;) {
					const int r = avcodec_receive_frame(codec.get(), frame.get());
					if (r == 0) {
						if (seekTarget > 0.0 && FrameTime() < seekTarget - 0.5 * std::max(1.0 / 120.0, frameRate > 0 ? 1.0 / frameRate : 0.0)) {
							av_frame_unref(frame.get());  // before the place we were asked to seek to
							continue;
						}
						seekTarget = 0.0;
						const bool done = Deliver(a_out, a_skipBefore);
						av_frame_unref(frame.get());
						if (done) {
							return true;
						}
						continue;
					}
					if (r == AVERROR_EOF) {
						return false;
					}
					if (r != AVERROR(EAGAIN)) {
						FFLOG("receive_frame failed: {}", Err(r));
						return false;
					}
					// the decoder wants more input
					if (draining) {
						return false;
					}
					const int rd = av_read_frame(fmt.get(), packet.get());
					if (rd < 0) {
						draining = true;
						avcodec_send_packet(codec.get(), nullptr);  // flush what is still inside
						continue;
					}
					if (packet->stream_index == stream) {
						const int sr = avcodec_send_packet(codec.get(), packet.get());
						if (sr < 0 && sr != AVERROR(EAGAIN) && sr != AVERROR_INVALIDDATA) {
							av_packet_unref(packet.get());
							FFLOG("send_packet failed: {}", Err(sr));
							return false;
						}
					}
					av_packet_unref(packet.get());
				}
			}

			void Rewind() override { Seek(0.0); }

			void Seek(double a_seconds) override
			{
				const double  t = std::max(0.0, a_seconds);
				const int64_t ts = static_cast<int64_t>((t + startTime) / av_q2d(timeBase));
				const bool    ok = av_seek_frame(fmt.get(), stream, ts, AVSEEK_FLAG_BACKWARD) >= 0;
				if (!ok) {
					// a format that can't seek (some GIFs, streams): start the file again
					if (!Open()) {
						return;
					}
				} else {
					avcodec_flush_buffers(codec.get());
				}
				draining = false;
				seekTarget = t;  // the keyframe we landed on can be well before it: Next() passes over frames until here
			}

			double FrameRate() const override { return frameRate; }
			double Duration() const override { return duration; }
			int    Width() const override { return outW; }
			int    Height() const override { return outH; }

		private:
			bool Open()
			{
				sws.reset();
				swsKey = {};
				codec.reset();
				fmt = OpenFormat(path);
				if (!fmt) {
					return false;
				}
				stream = FindVideoStream(fmt.get());
				if (stream < 0) {
					return false;
				}
				AVStream* st = fmt->streams[stream];
				// frames that are never used are dropped by the demuxer
				for (unsigned i = 0; i < fmt->nb_streams; ++i) {
					if (static_cast<int>(i) != stream) {
						fmt->streams[i]->discard = AVDISCARD_ALL;
					}
				}
				codec = OpenCodec(st, true);
				if (!codec) {
					return false;
				}
				frame.reset(av_frame_alloc());
				packet.reset(av_packet_alloc());
				if (!frame || !packet) {
					return false;
				}
				timeBase = st->time_base;
				startTime = st->start_time != AV_NOPTS_VALUE ? st->start_time * av_q2d(timeBase) : 0.0;
				if (startTime < 0.0) {
					startTime = 0.0;
				}
				{
					AVRational fr = av_guess_frame_rate(fmt.get(), st, nullptr);
					frameRate = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0.0;
				}
				srcW = codec->width;
				srcH = codec->height;
				if (srcW <= 0 || srcH <= 0) {
					return false;
				}
				outW = srcW;
				outH = srcH;
				if (aspect > 0 && maxWidth > 0) {
					// the part of the frame the canvas shows is cw wide: scale so that part is maxWidth wide
					const float cw = std::min(static_cast<float>(srcW), srcH * aspect);
					const float k = static_cast<float>(maxWidth) / cw;
					if (k < 0.9f) {
						const int sw = (static_cast<int>(srcW * k) + 1) & ~1, sh = (static_cast<int>(srcH * k) + 1) & ~1;
						if (sw >= 16 && sh >= 16) {
							outW = sw;
							outH = sh;
						}
					}
				}
				duration = DurationOf(st);
				draining = false;
				seekTarget = 0.0;
				lastTime = 0.0;
				return true;
			}

			// Container duration, else the stream's, else (GIF and friends) the end of the last packet.
			double DurationOf(AVStream* a_st)
			{
				double d = 0.0;
				if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
					d = fmt->duration / static_cast<double>(AV_TIME_BASE);
				} else if (a_st->duration != AV_NOPTS_VALUE && a_st->duration > 0) {
					d = a_st->duration * av_q2d(a_st->time_base);
				}
				if (d > 0.0) {
					return d;
				}
				// unknown: read through the packets once (no decoding, cheap) and note where the last one ends
				double end = 0.0;
				while (av_read_frame(fmt.get(), packet.get()) >= 0) {
					if (packet->stream_index == stream && packet->pts != AV_NOPTS_VALUE) {
						const double e = (packet->pts + std::max<int64_t>(packet->duration, 0)) * av_q2d(timeBase) - startTime;
						end = std::max(end, e);
					}
					av_packet_unref(packet.get());
				}
				av_seek_frame(fmt.get(), stream, 0, AVSEEK_FLAG_BACKWARD);
				avcodec_flush_buffers(codec.get());
				return end;
			}

			// One decoded frame -> a_out. False: it is a frame to skip (nothing delivered).
			// the current frame's time in seconds from the start of the video
			double FrameTime() const
			{
				int64_t pts = frame->best_effort_timestamp;
				if (pts == AV_NOPTS_VALUE) {
					pts = frame->pts;
				}
				if (pts == AV_NOPTS_VALUE) {
					return lastTime + (frameRate > 0 ? 1.0 / frameRate : 0.04);
				}
				return std::max(0.0, pts * av_q2d(timeBase) - startTime);
			}

			bool Deliver(Frame& a_out, double a_skipBefore)
			{
				const double t = FrameTime();
				lastTime = t;
				a_out.time = t;
				a_out.w = outW;
				a_out.h = outH;
				if (t < a_skipBefore) {
					a_out.bgra.clear();  // decoded (it has to be) but not converted: that is the expensive half
					return true;
				}
				sws.reset(sws_getCachedContext(sws.release(), frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), outW, outH,
					AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr));
				if (!sws) {
					return false;
				}
				if (const auto key = std::tuple{ frame->width, frame->height, frame->format, static_cast<int>(frame->colorspace),
						static_cast<int>(frame->color_range), outW, outH };
					key != swsKey) {  // (re)made: the colour set-up is per context, and is not cheap enough to repeat every frame
					swsKey = key;
					SetColourDetails(sws.get(), frame.get());
				}
				a_out.bgra.resize(static_cast<size_t>(outW) * outH * 4);
				std::uint8_t* dst[4]{ a_out.bgra.data(), nullptr, nullptr, nullptr };
				int           dstStride[4]{ outW * 4, 0, 0, 0 };
				sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, dst, dstStride);
				for (size_t i = 3; i < a_out.bgra.size(); i += 4) {  // opaque (GIF / WebP transparency would otherwise show as black holes)
					a_out.bgra[i] = 255;
				}
				return true;
			}

			std::wstring path;
			float        aspect{ 0 };
			int          maxWidth{ 0 };

			FormatPtr fmt;
			CodecPtr  codec;
			FramePtr  frame;
			PacketPtr packet;
			SwsPtr    sws;
			int       stream{ -1 };
			AVRational timeBase{ 1, 1000 };
			double    startTime{ 0 };
			double    duration{ 0 };
			double    frameRate{ 0 };
			double    lastTime{ 0 };
			int       srcW{ 0 }, srcH{ 0 };
			int       outW{ 0 }, outH{ 0 };
			bool      draining{ false };
			double    seekTarget{ 0 };
			std::tuple<int, int, int, int, int, int, int> swsKey{};
		};

		// Fold n channels down to mono/stereo with libswresample's own matrix (centre and surrounds mixed in).
		bool SetupResampler(SwrPtr& a_swr, const AVCodecContext* a_ctx, int& a_outChannels)
		{
			a_outChannels = std::min(a_ctx->ch_layout.nb_channels, 2);
			AVChannelLayout out{};
			av_channel_layout_default(&out, a_outChannels);
			AVChannelLayout in{};
			if (a_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
				av_channel_layout_default(&in, a_ctx->ch_layout.nb_channels);
			} else {
				av_channel_layout_copy(&in, &a_ctx->ch_layout);
			}
			SwrContext* raw = nullptr;
			const int   r = swr_alloc_set_opts2(&raw, &out, AV_SAMPLE_FMT_S16, a_ctx->sample_rate, &in, a_ctx->sample_fmt, a_ctx->sample_rate, 0, nullptr);
			av_channel_layout_uninit(&in);
			av_channel_layout_uninit(&out);
			a_swr.reset(raw);
			return r >= 0 && a_swr && swr_init(a_swr.get()) >= 0;
		}
	}

	std::unique_ptr<Decoder> Open(const std::wstring& a_path, float a_aspect, int a_maxWidth)
	{
		auto d = std::make_unique<FFDecoder>();
		return d->Init(a_path, a_aspect, a_maxWidth) ? std::move(d) : nullptr;
	}

	bool ReadAudio(const std::wstring& a_path, const std::atomic<bool>* a_run, Audio& a_out)
	{
		a_out = {};
		auto fmt = OpenFormat(a_path);
		if (!fmt) {
			return false;
		}
		const int stream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (stream < 0) {
			return false;
		}
		for (unsigned i = 0; i < fmt->nb_streams; ++i) {
			if (static_cast<int>(i) != stream) {
				fmt->streams[i]->discard = AVDISCARD_ALL;  // don't demux the picture just to throw it away
			}
		}
		auto codec = OpenCodec(fmt->streams[stream], false);
		if (!codec || codec->sample_rate <= 0 || codec->ch_layout.nb_channels <= 0) {
			return false;
		}
		SwrPtr swr;
		int    outCh = 0;
		if (!SetupResampler(swr, codec.get(), outCh)) {
			FFLOG("audio resampler setup failed");
			return false;
		}
		Audio out;
		out.channels = outCh;
		out.rate = codec->sample_rate;

		FramePtr  frame(av_frame_alloc());
		PacketPtr packet(av_packet_alloc());
		std::vector<std::int16_t> buf;

		auto convert = [&](const std::uint8_t** a_in, int a_inSamples) {
			const int cap = swr_get_out_samples(swr.get(), a_inSamples);
			if (cap <= 0) {
				return;
			}
			buf.resize(static_cast<size_t>(cap) * outCh);
			std::uint8_t* o[1]{ reinterpret_cast<std::uint8_t*>(buf.data()) };
			const int     n = swr_convert(swr.get(), o, cap, a_in, a_inSamples);
			if (n > 0) {
				out.samples.insert(out.samples.end(), buf.begin(), buf.begin() + static_cast<size_t>(n) * outCh);
			}
		};
		auto drainDecoder = [&]() {
			while (avcodec_receive_frame(codec.get(), frame.get()) == 0) {
				convert(const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
				av_frame_unref(frame.get());
			}
		};

		while (av_read_frame(fmt.get(), packet.get()) >= 0) {
			if (a_run && !a_run->load()) {
				av_packet_unref(packet.get());
				return false;  // the video was stopped: no use finishing its sound
			}
			if (packet->stream_index == stream && avcodec_send_packet(codec.get(), packet.get()) >= 0) {
				drainDecoder();
			}
			av_packet_unref(packet.get());
		}
		avcodec_send_packet(codec.get(), nullptr);
		drainDecoder();
		convert(nullptr, 0);  // what the resampler still holds

		if (out.samples.empty()) {
			return false;
		}
		a_out = std::move(out);
		return true;
	}

	bool StillSize(const std::wstring& a_path, int& a_w, int& a_h)
	{
		auto fmt = OpenFormat(a_path);
		if (!fmt) {
			return false;
		}
		const int s = FindVideoStream(fmt.get());
		if (s < 0) {
			return false;
		}
		a_w = fmt->streams[s]->codecpar->width;
		a_h = fmt->streams[s]->codecpar->height;
		return a_w > 0 && a_h > 0;
	}

	bool LoadStill(const std::wstring& a_path, int& a_w, int& a_h, std::vector<std::uint8_t>& a_rgba)
	{
		auto fmt = OpenFormat(a_path);
		if (!fmt) {
			return false;
		}
		const int s = FindVideoStream(fmt.get());
		if (s < 0) {
			return false;
		}
		auto codec = OpenCodec(fmt->streams[s], false);
		if (!codec) {
			return false;
		}
		FramePtr  frame(av_frame_alloc());
		PacketPtr packet(av_packet_alloc());
		bool      got = false, flushed = false;
		while (!got) {
			int r = avcodec_receive_frame(codec.get(), frame.get());
			if (r == 0) {
				got = true;
				break;
			}
			if (r != AVERROR(EAGAIN) || flushed) {
				break;
			}
			if (av_read_frame(fmt.get(), packet.get()) < 0) {
				flushed = true;
				avcodec_send_packet(codec.get(), nullptr);
				continue;
			}
			if (packet->stream_index == s) {
				avcodec_send_packet(codec.get(), packet.get());
			}
			av_packet_unref(packet.get());
		}
		if (!got || frame->width <= 0 || frame->height <= 0) {
			return false;
		}
		SwsPtr sws(sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
			AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr));
		if (!sws) {
			return false;
		}
		SetColourDetails(sws.get(), frame.get());
		a_w = frame->width;
		a_h = frame->height;
		a_rgba.assign(static_cast<size_t>(a_w) * a_h * 4, 255);
		std::uint8_t* dst[4]{ a_rgba.data(), nullptr, nullptr, nullptr };
		int           dstStride[4]{ a_w * 4, 0, 0, 0 };
		sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, dst, dstStride);
		return true;
	}
}
