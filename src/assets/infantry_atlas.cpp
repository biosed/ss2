#include "assets/infantry_atlas.hpp"
#include <algorithm>
#include <fstream>
#include <png.h>
#include <stdexcept>

namespace ss2::assets {
namespace {
constexpr int cell = 64;
constexpr int footX = 32, footY = 58;
struct Png {
    png_image image{};
    Png() { image.version = PNG_IMAGE_VERSION; }
    ~Png() { png_image_free(&image); }
};

InfantryPose extract(const MapImage& sheet, int column, int row) {
    const auto pixel = [&](int x, int y) {
        return sheet.rgba.data() + ((row*cell + y)*sheet.width + column*cell + x)*4;
    };
    int left = cell, top = cell, right = 0, bottom = 0;
    for (int y = 0; y < cell; ++y) {
        for (int x = 0; x < cell; ++x) {
            if (pixel(x,y)[3] < 96) continue;
            left = std::min(left,x); top = std::min(top,y);
            right = std::max(right,x+1); bottom = std::max(bottom,y+1);
        }
    }
    if (!right) {
        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < cell; ++x) {
                if (!pixel(x,y)[3]) continue;
                left = std::min(left,x); top = std::min(top,y);
                right = std::max(right,x+1); bottom = std::max(bottom,y+1);
            }
        }
    }
    if (!right) throw std::runtime_error("Empty infantry atlas cell");
    // Round outwards to an exact number of logical map pixels at 2x.
    left &= ~1; top &= ~1;
    right = (right+1) & ~1; bottom = (bottom+1) & ~1;
    MapImage raster{unsigned(right-left),unsigned(bottom-top),{}};
    raster.rgba.reserve(raster.width*raster.height*4);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const auto* p = pixel(x,y);
            for (int c = 0; c < 3; ++c) raster.rgba.push_back((p[c]*p[3]+127)/255);
            raster.rgba.push_back(p[3]);
        }
    }
    return {(left-footX)/2, (top-footY)/2, (right-left)/2, (bottom-top)/2, std::move(raster)};
}
}

InfantryPoses loadInfantryAtlas(const std::filesystem::path& path) {
    Png png;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > 2*1024*1024) throw std::runtime_error("Invalid infantry atlas size: " + path.string());
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size) ||
        !png_image_begin_read_from_memory(&png.image, bytes.data(), bytes.size()))
        throw std::runtime_error("Cannot read infantry atlas " + path.string());
    if (png.image.width != 8*cell || png.image.height != 5*cell)
        throw std::runtime_error("Expected a 512x320 infantry atlas in " + path.string());
    png.image.format = PNG_FORMAT_RGBA;
    MapImage sheet{png.image.width,png.image.height,std::vector<std::uint8_t>(PNG_IMAGE_SIZE(png.image))};
    if (!png_image_finish_read(&png.image,nullptr,sheet.rgba.data(),0,nullptr))
        throw std::runtime_error("Invalid PNG data in " + path.string());
    InfantryPoses poses;
    for (int row = 0; row < 5; ++row)
        for (int dir = 0; dir < 8; ++dir)
            poses[(row ? 8 : 0) + dir].push_back(extract(sheet,dir,row));
    return poses;
}

MapImage loadPngRgba(const std::filesystem::path& path) {
    Png png;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > 8*1024*1024) throw std::runtime_error("Invalid PNG size: " + path.string());
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size) ||
        !png_image_begin_read_from_memory(&png.image, bytes.data(), bytes.size()))
        throw std::runtime_error("Cannot read PNG " + path.string());
    png.image.format = PNG_FORMAT_RGBA;
    MapImage image{png.image.width, png.image.height, std::vector<std::uint8_t>(PNG_IMAGE_SIZE(png.image))};
    if (!png_image_finish_read(&png.image, nullptr, image.rgba.data(), 0, nullptr))
        throw std::runtime_error("Invalid PNG data in " + path.string());
    return image;
}
}
