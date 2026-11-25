#include "assets/sprite.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) try {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc != 4) throw std::invalid_argument("Usage: ss2_sprite ARCHIVE ENTRY FRAME > pixels.le16");
    const std::string_view text(argv[3]);
    size_t index{};
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), index);
    if (ec != std::errc{} || end != text.data() + text.size()) throw std::invalid_argument("Invalid frame number");
    const auto sprite = ss2::assets::decodePckFrame(ss2::assets::readSueEntry(argv[1], argv[2]), index);
    std::cerr << "frame=" << index << " origin=" << sprite.x << ',' << sprite.y << " size="
              << sprite.width << 'x' << sprite.height << '\n';
    std::cout.write(reinterpret_cast<const char*>(sprite.pixels.data()), sprite.pixels.size()).flush();
    return std::cout ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
