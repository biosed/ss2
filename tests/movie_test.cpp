#include "video/movie_player.hpp"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    using ss2::letterboxRect;
    auto r = letterboxRect(640, 300, 1280, 720);
    assert(r.x == 0 && r.y == 60 && r.w == 1280 && r.h == 600);
    r = letterboxRect(640, 480, 1280, 720);
    assert(r.x == 160 && r.y == 0 && r.w == 960 && r.h == 720);
    r = letterboxRect(640, 300, 640, 300);
    assert(r.x == 0 && r.y == 0 && r.w == 640 && r.h == 300);
    r = letterboxRect(1280, 720, 640, 360);
    assert(r.x == 0 && r.y == 0 && r.w == 640 && r.h == 360);
    r = letterboxRect(640, 480, 800, 600);
    assert(r.x == 0 && r.y == 0 && r.w == 800 && r.h == 600);
    r = letterboxRect(0, 0, 100, 100);
    assert(r.w == 0 && r.h == 0);
    std::cout << "letterbox math PASSED\n";

    if (argc > 1) {
        const std::filesystem::path dir = std::filesystem::path(argv[1]) / "movies";
        ss2::MovieInfo info;
        assert(ss2::probeMovie(dir / "intro.BIK", info));
        assert(info.hasVideo && info.width == 640 && info.height == 300);
        assert(std::abs(info.fps - 24.0) < 0.01);
        assert(info.hasAudio && info.sampleRate == 44100 && info.channels == 2);
        assert(std::abs(info.duration - 102.25) < 0.5);
        assert(ss2::probeMovie(dir / "logo1.BIK", info));
        assert(info.hasVideo && info.width == 640 && info.height == 480);
        assert(std::abs(info.fps - 25.0) < 0.01);
        assert(info.hasAudio && info.sampleRate == 22050 && info.channels == 2);
        assert(std::abs(info.duration - 14.0) < 0.5);
        ss2::MovieInfo missing;
        assert(!ss2::probeMovie(dir / "does-not-exist.BIK", missing));
        // Headless (no SDL video subsystem): playMovie must be a safe no-op.
        assert(!ss2::playMovie(dir / "intro.BIK", false, true));
        std::cout << "gold movie decode PASSED (intro 640x300@24, logo1 640x480@25)\n";
    }
    return 0;
}