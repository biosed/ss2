#include "video/movie_player.hpp"
#include "assets/sprite.hpp"
#include <SDL3/SDL.h>
#ifdef SS2_HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
}
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace ss2 {
#ifdef SS2_HAVE_FFMPEG
namespace {

struct FormatDeleter { void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); } };
struct CodecDeleter { void operator()(AVCodecContext* p) const { if (p) avcodec_free_context(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { if (p) av_frame_free(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { if (p) av_packet_free(&p); } };
struct SwsDeleter { void operator()(SwsContext* p) const { if (p) sws_freeContext(p); } };
struct SwrDeleter { void operator()(SwrContext* p) const { if (p) swr_free(&p); } };
using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

struct Streams {
    FormatPtr fmt;
    int video = -1, audio = -1;
};

bool openMovie(const std::filesystem::path& file, Streams& s) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, file.string().c_str(), nullptr, nullptr) < 0) return false;
    s.fmt.reset(raw);
    if (avformat_find_stream_info(s.fmt.get(), nullptr) < 0) return false;
    s.video = av_find_best_stream(s.fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    s.audio = av_find_best_stream(s.fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    return s.video >= 0 || s.audio >= 0;
}

CodecPtr openDecoder(AVFormatContext* fmt, int index) {
    if (index < 0) return nullptr;
    const AVStream* st = fmt->streams[index];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) return nullptr;
    CodecPtr ctx(avcodec_alloc_context3(dec));
    if (!ctx) return nullptr;
    if (avcodec_parameters_to_context(ctx.get(), st->codecpar) < 0) return nullptr;
    if (avcodec_open2(ctx.get(), dec, nullptr) < 0) return nullptr;
    return ctx;
}

double frameSeconds(const AVFrame* f, const AVRational tb) {
    const std::int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    return pts == AV_NOPTS_VALUE ? 0.0 : double(pts) * av_q2d(tb);
}

} // namespace
#endif

MovieRect letterboxRect(int vw, int vh, int ww, int wh) {
    if (vw <= 0 || vh <= 0 || ww <= 0 || wh <= 0) return {0, 0, 0, 0};
    const double scale = std::min(double(ww) / vw, double(wh) / vh);
    const int w = std::max(1, int(std::lround(vw * scale)));
    const int h = std::max(1, int(std::lround(vh * scale)));
    return {(ww - w) / 2, (wh - h) / 2, w, h};
}

bool probeMovie(const std::filesystem::path& file, MovieInfo& info) {
    info = {};
#ifndef SS2_HAVE_FFMPEG
    (void)file;
    return false;
#else
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return false;
    Streams s;
    if (!openMovie(file, s)) return false;
    if (s.video >= 0) {
        const AVStream* st = s.fmt->streams[s.video];
        info.hasVideo = true;
        info.width = st->codecpar->width;
        info.height = st->codecpar->height;
        if (st->avg_frame_rate.den) info.fps = av_q2d(st->avg_frame_rate);
        else if (st->r_frame_rate.den) info.fps = av_q2d(st->r_frame_rate);
    }
    if (s.audio >= 0) {
        const AVStream* st = s.fmt->streams[s.audio];
        info.hasAudio = true;
        info.sampleRate = st->codecpar->sample_rate;
        info.channels = st->codecpar->ch_layout.nb_channels;
    }
    if (s.fmt->duration > 0) info.duration = double(s.fmt->duration) / AV_TIME_BASE;
    if (s.video >= 0) {
        auto dec = openDecoder(s.fmt.get(), s.video);
        if (!dec) return false;
        FramePtr frame(av_frame_alloc());
        PacketPtr pkt(av_packet_alloc());
        int decoded = 0;
        while (decoded < 3 && av_read_frame(s.fmt.get(), pkt.get()) >= 0) {
            if (pkt->stream_index == s.video && avcodec_send_packet(dec.get(), pkt.get()) >= 0) {
                while (avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                    ++decoded;
                    av_frame_unref(frame.get());
                    if (decoded >= 3) break;
                }
            }
            av_packet_unref(pkt.get());
        }
        if (decoded == 0) return false;
    }
    return info.hasVideo || info.hasAudio;
#endif
}

#ifdef SS2_HAVE_FFMPEG
namespace {
struct WindowDeleter { void operator()(SDL_Window* p) const { if (p) SDL_DestroyWindow(p); } };
struct RendererDeleter { void operator()(SDL_Renderer* p) const { if (p) SDL_DestroyRenderer(p); } };
struct TextureDeleter { void operator()(SDL_Texture* p) const { if (p) SDL_DestroyTexture(p); } };
struct StreamDeleter { void operator()(SDL_AudioStream* p) const { if (p) SDL_DestroyAudioStream(p); } };
using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;
using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;
using AudioStreamPtr = std::unique_ptr<SDL_AudioStream, StreamDeleter>;
} // namespace
#endif

bool playMovie(const std::filesystem::path& file, bool fullscreen, bool allowSkip) {
#ifndef SS2_HAVE_FFMPEG
    (void)file;
    (void)fullscreen;
    (void)allowSkip;
    return false;
#else
    MovieInfo info;
    if (!probeMovie(file, info) || !info.hasVideo) return false;
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || SDL_strcmp(driver, "offscreen") == 0 || SDL_strcmp(driver, "dummy") == 0) return false;

    Streams s;
    if (!openMovie(file, s)) return false;
    auto vdec = openDecoder(s.fmt.get(), s.video);
    if (!vdec) return false;
    const AVRational vtb = s.fmt->streams[s.video]->time_base;
    CodecPtr adec = openDecoder(s.fmt.get(), s.audio);
    const AVRational atb = s.audio >= 0 ? s.fmt->streams[s.audio]->time_base : AVRational{1, 1};

    FramePtr vframe(av_frame_alloc()), aframe(av_frame_alloc());
    PacketPtr pkt(av_packet_alloc());
    if (!vframe || !aframe || !pkt) return false;
    SwsPtr sws(sws_getContext(info.width, info.height, vdec->pix_fmt, info.width, info.height,
                              AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) return false;
    std::vector<std::uint8_t> rgb(std::size_t(info.width) * std::size_t(info.height) * 3);

    SwrPtr swr;
    int audioRate = 0, audioCh = 0;
    std::int64_t samplesPut = 0;
    double audioBase = 0;
    bool audioBaseSet = false;
    if (adec) {
        audioRate = adec->sample_rate;
        audioCh = adec->ch_layout.nb_channels;
        SwrContext* raw = nullptr;
        AVChannelLayout out;
        av_channel_layout_default(&out, audioCh);
        if (swr_alloc_set_opts2(&raw, &out, AV_SAMPLE_FMT_S16, audioRate,
                                &adec->ch_layout, adec->sample_fmt, audioRate, 0, nullptr) >= 0 &&
            swr_init(raw) >= 0) {
            swr.reset(raw);
        } else if (raw) {
            swr_free(&raw);
        }
        av_channel_layout_uninit(&out);
    }
    const int bytesPerFrame = 2 * std::max(1, audioCh);
    std::vector<std::uint8_t> audioBuf(std::size_t(bytesPerFrame) * 192000);

    int winW = info.width, winH = info.height;
    SDL_Rect work{};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &work) && work.w > 0 && work.h > 0) {
        const double scale = std::min(double(work.w) * 0.8 / info.width, double(work.h) * 0.8 / info.height);
        winW = std::max(1, int(std::lround(info.width * scale)));
        winH = std::max(1, int(std::lround(info.height * scale)));
    }
    SDL_WindowFlags flags = fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* winRaw = nullptr;
    SDL_Renderer* renRaw = nullptr;
    if (!SDL_CreateWindowAndRenderer("Sudden Strike II", winW, winH, flags, &winRaw, &renRaw)) return false;
    WindowPtr window(winRaw);
    RendererPtr renderer(renRaw);
    TexturePtr texture(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                         info.width, info.height));
    if (!texture) return false;
    SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_LINEAR);

    SDL_AudioDeviceID device = 0;
    AudioStreamPtr audioStream;
    if (swr && (SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO))) {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;
        spec.channels = audioCh;
        spec.freq = audioRate;
        device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
        if (device) {
            audioStream.reset(SDL_CreateAudioStream(&spec, &spec));
            if (audioStream) {
                SDL_BindAudioStream(device, audioStream.get());
                SDL_ResumeAudioDevice(device);
            } else {
                SDL_CloseAudioDevice(device);
                device = 0;
            }
        }
    }

    const std::uint64_t startTicks = SDL_GetTicks();
    bool skipped = false;
    auto masterClock = [&]() -> double {
        if (audioStream && audioBaseSet) {
            const int queued = SDL_GetAudioStreamAvailable(audioStream.get());
            const double played = double(samplesPut - queued / bytesPerFrame) / audioRate;
            return audioBase + std::max(0.0, played);
        }
        return double(SDL_GetTicks() - startTicks) / 1000.0;
    };
    auto pollSkip = [&]() -> bool {
        if (!allowSkip) return false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) return true;
            if (e.type == SDL_EVENT_KEY_DOWN &&
                (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_SPACE ||
                 e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) return true;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) return true;
        }
        return false;
    };
    auto presentFrame = [&](AVFrame* f) {
        const double pts = frameSeconds(f, vtb);
        for (;;) {
            if (pollSkip()) { skipped = true; return; }
            const double diff = pts - masterClock();
            if (diff <= 0.010) break;
            SDL_Delay(Uint32(std::min(diff, 0.05) * 1000.0));
        }
        std::uint8_t* dst[4] = {rgb.data(), nullptr, nullptr, nullptr};
        const int dstLines[4] = {info.width * 3, 0, 0, 0};
        sws_scale(sws.get(), f->data, f->linesize, 0, info.height, dst, dstLines);
        SDL_UpdateTexture(texture.get(), nullptr, rgb.data(), info.width * 3);
        int outW = 0, outH = 0;
        SDL_GetRenderOutputSize(renderer.get(), &outW, &outH);
        const MovieRect r = letterboxRect(info.width, info.height, outW, outH);
        SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255);
        SDL_RenderClear(renderer.get());
        const SDL_FRect dr{float(r.x), float(r.y), float(r.w), float(r.h)};
        SDL_RenderTexture(renderer.get(), texture.get(), nullptr, &dr);
        SDL_RenderPresent(renderer.get());
    };
    auto feedAudio = [&](AVFrame* f) {
        if (!swr || !audioStream) return;
        if (!audioBaseSet) { audioBase = frameSeconds(f, atb); audioBaseSet = true; }
        std::uint8_t* out[1] = {audioBuf.data()};
        const int capacity = int(audioBuf.size()) / bytesPerFrame;
        const int n = swr_convert(swr.get(), out, capacity,
                                  const_cast<const std::uint8_t**>(f->extended_data), f->nb_samples);
        if (n > 0) {
            SDL_PutAudioStreamData(audioStream.get(), audioBuf.data(), n * bytesPerFrame);
            samplesPut += n;
        }
    };
    auto drainVideo = [&]() {
        while (avcodec_receive_frame(vdec.get(), vframe.get()) >= 0) {
            presentFrame(vframe.get());
            av_frame_unref(vframe.get());
            if (skipped) return;
        }
    };
    while (!skipped && av_read_frame(s.fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == s.video) {
            if (avcodec_send_packet(vdec.get(), pkt.get()) >= 0) drainVideo();
        } else if (pkt->stream_index == s.audio && adec) {
            if (avcodec_send_packet(adec.get(), pkt.get()) >= 0) {
                while (avcodec_receive_frame(adec.get(), aframe.get()) >= 0) {
                    feedAudio(aframe.get());
                    av_frame_unref(aframe.get());
                }
            }
        }
        av_packet_unref(pkt.get());
    }
    if (!skipped) {
        avcodec_send_packet(vdec.get(), nullptr);
        drainVideo();
        if (adec) {
            avcodec_send_packet(adec.get(), nullptr);
            while (avcodec_receive_frame(adec.get(), aframe.get()) >= 0) {
                feedAudio(aframe.get());
                av_frame_unref(aframe.get());
            }
            if (swr && audioStream) {
                std::uint8_t* out[1] = {audioBuf.data()};
                const int capacity = int(audioBuf.size()) / bytesPerFrame;
                int n;
                while ((n = swr_convert(swr.get(), out, capacity, nullptr, 0)) > 0) {
                    SDL_PutAudioStreamData(audioStream.get(), audioBuf.data(), n * bytesPerFrame);
                    samplesPut += n;
                }
            }
        }
    }
    if (audioStream && !skipped) {
        const std::uint64_t drainStart = SDL_GetTicks();
        while (SDL_GetAudioStreamAvailable(audioStream.get()) > 0 && SDL_GetTicks() - drainStart < 3000) {
            if (pollSkip()) { skipped = true; break; }
            SDL_Delay(10);
        }
    }
    audioStream.reset();
    if (device) SDL_CloseAudioDevice(device);
    return !skipped;
#endif
}

namespace {
std::filesystem::path defaultHdPackDir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "ss2/hd-pack";
#ifdef _WIN32
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData)
        return std::filesystem::path(localAppData) / "ss2/hd-pack";
    if (const char* appData = std::getenv("APPDATA"); appData && *appData)
        return std::filesystem::path(appData) / "ss2/hd-pack";
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile)
        return std::filesystem::path(profile) / "AppData/Local/ss2/hd-pack";
#else
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local/share/ss2/hd-pack";
#endif
    return {};
}
} // namespace

void playOpeningMovies(const std::filesystem::path& gameDir, bool fullscreen,
                       const std::filesystem::path& hdPack) {
    if (gameDir.empty()) return;
    const auto pack = hdPack.empty() ? defaultHdPackDir() : hdPack;
    for (const char* stem : {"logo1", "intro"}) {
        std::error_code ec;
        if (!pack.empty()) {
            const auto hd = pack / "movies-hd" / (std::string(stem) + ".mkv");
            if (std::filesystem::is_regular_file(hd, ec)) {
                std::cout << "Opening movie (HD upscaled): " << hd.filename().string() << '\n';
                playMovie(hd, fullscreen, true);
                continue;
            }
        }
        const auto movieDirs = assets::resolveMovieDirs(gameDir);
        for (const auto& dir : movieDirs) {
            const auto path = assets::resolveCaseInsensitivePath(dir, std::string(stem) + ".BIK");
            if (std::filesystem::exists(path, ec)) {
                playMovie(path, fullscreen, true);
                break;
            }
        }
    }
}
} // namespace ss2