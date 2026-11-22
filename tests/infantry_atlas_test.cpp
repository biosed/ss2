#include "assets/infantry.hpp"
#include "assets/infantry_atlas.hpp"
#include <algorithm>
#include <png.h>
#include <iostream>
#include <stdexcept>

int main() try {
    const auto path = std::filesystem::current_path() / "infantry-test-atlas.png";
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); }
    } cleanup{path};
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = 512; png.height = 320; png.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> sheet(PNG_IMAGE_SIZE(png));
    for (unsigned row = 0; row < 5; ++row) {
        for (unsigned col = 0; col < 8; ++col) {
            auto* p = sheet.data() + ((row*64+55)*png.width+col*64+33)*4;
            p[0] = 2*(row+col); p[1] = 240; p[3] = 128;
        }
    }
    const auto write = [&] {
        if (!png_image_write_to_file(&png,path.filename().string().c_str(),0,sheet.data(),0,nullptr))
            throw std::runtime_error("Cannot write test PNG");
    };
    write();
    auto poses = ss2::assets::loadInfantryAtlas(path);
    for (unsigned row = 0; row < 5; ++row) {
        for (unsigned col = 0; col < 8; ++col) {
            const auto& frames = poses[col+(row ? 8 : 0)];
            const auto& frame = frames.at(row ? row-1 : 0);
            if (frames.size() != (row ? 4u : 1u) || frame.left != 0 || frame.top != -2 ||
                frame.width != 1 || frame.height != 1 ||
                frame.pixels.rgba != std::vector<std::uint8_t>{0,0,0,0, 0,0,0,0, 0,0,0,0,
                    std::uint8_t(row+col),120,0,128}) throw std::runtime_error("Incorrect pose");
        }
    }
    // Asset-free InfantryScene visibility (visible=0) & death (hp=0) pixel check (runs in hosted CI).
    {
        const auto dir = std::filesystem::current_path() / "infantry-scene-ci-test";
        std::filesystem::create_directories(dir);
        struct DirCleanup {
            std::filesystem::path dir;
            ~DirCleanup() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
        } dirCleanup{dir};
        std::filesystem::copy_file(path, dir / "dguner.png", std::filesystem::copy_options::overwrite_existing);

        ss2::assets::MapImage bg{32, 32, std::vector<std::uint8_t>(32 * 32 * 4, 20)};
        ss2::assets::MapImage hd{64, 64, std::vector<std::uint8_t>(64 * 64 * 4, 20)};
        const std::vector<ss2::assets::Placement> placements{
            {"dguner", 10, 12, 100, 40, 0, 0, 0},
            {"dguner", 20, 22, 100, 40, 3, 1, 0},
        };
        ss2::assets::InfantryScene scene(bg, {}, placements, false, hd, dir, true);
        auto rendered = scene.compose();
        if (rendered.rgba == hd.rgba) throw std::runtime_error("Visible infantry did not paint");
        const auto apply = [](ss2::assets::MapImage& dst, const ss2::assets::MapPatch& patch) {
            for (unsigned r = 0; r < patch.pixels.height; ++r) {
                std::copy_n(patch.pixels.rgba.begin() + r * size_t(patch.pixels.width) * 4,
                            patch.pixels.width * 4,
                            dst.rgba.begin() + ((patch.y + r) * size_t(dst.width) + patch.x) * 4);
            }
        };
        apply(rendered, scene.update(0, 10, 12, 0, 0, false, 100, 0)); // hidden
        apply(rendered, scene.update(1, 20, 22, 0, 0, false, 0, 1));   // dead
        if (rendered.rgba != hd.rgba || scene.compose().rgba != hd.rgba)
            throw std::runtime_error("Hidden or dead infantry still painted in asset-free scene");

        // Fog-of-war shroud darkening check (asset-free CI coverage).
        std::vector<std::uint8_t> fog(128 * 128, 0);
        const auto fogPatch = scene.updateFog(fog);
        if (fogPatch.pixels.width != 64 || fogPatch.pixels.height != 64)
            throw std::runtime_error("Initial fog patch did not cover scene");
        if (fogPatch.pixels.rgba[0] >= hd.rgba[0])
            throw std::runtime_error("Unexplored fog shroud did not darken terrain");
        if (scene.compose().rgba != fogPatch.pixels.rgba)
            throw std::runtime_error("Fog compose differs from updateFog patch");
    }

    // Synthetic SoldClr uniform tinting check (n2Game_Dll.dll 0x1007ba80..0x1007bbde).
    {
        std::vector<std::uint8_t> basePl(512, 0);
        const auto put565 = [&](std::size_t idx, std::uint16_t w) {
            basePl[2 * idx] = static_cast<std::uint8_t>(w & 0xFFu);
            basePl[2 * idx + 1] = static_cast<std::uint8_t>((w >> 8) & 0xFFu);
        };
        const auto get565 = [](const std::vector<std::uint8_t>& pl, std::size_t idx) -> std::uint16_t {
            return static_cast<std::uint16_t>(pl[2 * idx] | (unsigned(pl[2 * idx + 1]) << 8));
        };
        put565(1, 0xCC29u); // saturated flesh (200, 132, 72) -> preserved
        put565(2, 0x9CD3u); // neutral grey uniform (152, 152, 152) -> tinted
        put565(3, 0xCE59u); // bright grey (200, 200, 200) -> saturates to white under 350

        if (ss2::assets::buildSoldClrPalette(basePl, -1, -1, -1) != basePl)
            throw std::runtime_error("German (-1,-1,-1) SoldClr must copy SOLD.PL verbatim");

        const auto rusPl = ss2::assets::buildSoldClrPalette(basePl, 240, 288, 180);
        if (get565(rusPl, 1) != 0xCC29u)
            throw std::runtime_error("SoldClr altered saturated flesh entry");
        if (get565(rusPl, 2) != 0x956Du)
            throw std::runtime_error("Russian SoldClr uniform tint mismatch");

        const auto medicPl = ss2::assets::buildSoldClrPalette(basePl, 350, 350, 350);
        if (get565(medicPl, 1) != 0xCC29u || get565(medicPl, 3) != 0xFFFFu)
            throw std::runtime_error("Medic SoldClr saturation clamp mismatch");

        bool threw = false;
        try {
            (void)ss2::assets::buildSoldClrPalette(std::span<const std::uint8_t>(basePl).first(256), 250, 250, 250);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) throw std::runtime_error("Accepted short SOLD.PL buffer");
    }

    sheet[((55*png.width+33)*4)+3] = 0;
    write();
    try { ss2::assets::loadInfantryAtlas(path); }
    catch (const std::runtime_error&) { return 0; }
    throw std::runtime_error("Accepted empty frame");
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
