#pragma once
#include <filesystem>

namespace ss2 {

// Stream properties of a Bink movie, read without opening a window or audio device.
struct MovieInfo {
    int width = 0, height = 0;
    double fps = 0;
    int sampleRate = 0, channels = 0;
    double duration = 0;
    bool hasVideo = false, hasAudio = false;
};

// Decode-only inspection. False when the file is missing or not a readable movie.
bool probeMovie(const std::filesystem::path& file, MovieInfo& info);

// Aspect-preserving destination rectangle (letterboxed/pillarboxed) centred in a window.
struct MovieRect { int x, y, w, h; };
MovieRect letterboxRect(int videoW, int videoH, int winW, int winH);

// Play one Bink movie in its own SDL window with audio-master A/V sync. Returns true when
// it played to completion, false when skipped or unavailable. A safe no-op (false) when the
// file is missing, the SDL video driver is headless, or no renderer/audio device exists.
bool playMovie(const std::filesystem::path& file, bool fullscreen, bool allowSkip);

// Retail opening sequence: logo1 then intro. When an HD pack supplies movies-hd/<name>.mkv
// (AI-upscaled), that is preferred over the retail movies/<name>.BIK; otherwise the original
// Bink is resolved case-insensitively. An empty hdPack falls back to the standard install dir.
void playOpeningMovies(const std::filesystem::path& gameDir, bool fullscreen,
                       const std::filesystem::path& hdPack = {});

} // namespace ss2