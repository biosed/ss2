#include "assets/infantry.hpp"
#include "assets/hd_pack.hpp"
#include "assets/sprite.hpp"
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace ss2::assets;

namespace {
MapImage ground() {
    MapImage map{144, 96, std::vector<std::uint8_t>(144*96*4, 255)};
    for (size_t i = 0; i < map.rgba.size(); i += 4) {
        map.rgba[i] = 62; map.rgba[i+1] = 75; map.rgba[i+2] = 44;
    }
    return map;
}

void apply(MapImage& image, const MapPatch& patch) {
    for (unsigned y = 0; y < patch.pixels.height; ++y)
        std::copy_n(patch.pixels.rgba.data()+y*size_t(patch.pixels.width)*4, patch.pixels.width*4,
                    image.rgba.data()+((patch.y+y)*size_t(image.width)+patch.x)*4);
}

void checkStandDepth(const std::filesystem::path& game, int scale) {
    // Each row is opaque at the left, with an explicitly transparent run at the right.
    std::vector<std::uint8_t> sprite{0,0,0,0,32,0,40,0,0xa1};
    for (int y = 0; y < 40; ++y) sprite.insert(sprite.end(), {4,0,0x90,1,0x90,0});
    std::vector<std::uint8_t> palette(512);
    auto base = ground();
    SpriteCoverage cover;
    drawIndexedFrame(base, 44, 26, sprite, palette, &cover);
    SceneryDepth depth(base.width,base.height);
    depth.addBody(cover,61);
    auto hd = scale == 2 ? upscalePatch2x(base) : MapImage{};
    const auto underlay = scale == 2 ? hd : base;
    const std::vector<Placement> actor{{"dofficer", 60, 60, 0, 100, 90, 0, 0}};
    InfantryScene plain(base, game, actor, true, hd, {}, true);
    InfantryScene scene(base, game, actor, true, hd, {}, true, std::move(depth));
    auto output = scene.compose();
    const auto reference = plain.compose();
    int hidden = 0, visible = 0;
    for (unsigned y = 0; y < output.height; ++y) {
        for (unsigned x = 0; x < output.width; ++x) {
            const auto i = (std::size_t(y)*output.width+x)*4;
            const bool covered = cover.covers(x/scale,y/scale);
            const auto& expected = covered ? underlay.rgba : reference.rgba;
            if (!std::equal(expected.begin()+i,expected.begin()+i+4,output.rgba.begin()+i))
                throw std::runtime_error("Soldier behind stand has incorrect coverage");
            if (!std::equal(reference.rgba.begin()+i,reference.rgba.begin()+i+4,underlay.rgba.begin()+i))
                ++(covered ? hidden : visible);
        }
    }
    if (!hidden || !visible) throw std::runtime_error("Fixture must exercise hidden pixels and transparent runs");
    const auto solid = output;
    for (const auto& patch : scene.setInspection({0},{},0)) apply(output,patch);
    if (output.rgba == solid.rgba || output.rgba != scene.compose().rgba)
        throw std::runtime_error("Selecting a hidden infantryman did not reveal the checkerboard");
    const auto selected = output;
    for (const auto& patch : scene.setInspection({},std::pair{60,60},0)) apply(output,patch);
    if (output.rgba != selected.rgba) throw std::runtime_error("Cursor and selection visibility differ");
    for (const auto& patch : scene.setInspection({},std::pair{60,60},1)) apply(output,patch);
    if (output.rgba == selected.rgba || output.rgba != scene.compose().rgba)
        throw std::runtime_error("Checkerboard parity did not follow the camera");
    for (const auto& patch : scene.setInspection({},std::pair{316,60},1)) apply(output,patch);
    if (output.rgba != solid.rgba) throw std::runtime_error("Cursor at distance 256 still reveals infantry");
    for (const int y : {62, 61, 60}) {
        apply(output, scene.update(0, 60, y, 0, 0, false));
        plain.update(0, 60, y, 0, 0, false);
        if (output.rgba != scene.compose().rgba || (y > 61 && output.rgba != plain.compose().rgba))
            throw std::runtime_error("Front/back stand crossing differs from expected redraw");
    }
}

void checkAssetDepth(const std::filesystem::path& game, bool tree) {
    const auto bytes = readSueEntry(game/"game_scheme1.sue",tree ? "tree.res" : "STAND.RES");
    const std::span<const std::uint8_t> data(bytes);
    const auto word32 = [](auto view, std::size_t pos) -> unsigned {
        if (pos+4 > view.size()) throw std::runtime_error("Truncated depth fixture");
        return unsigned(view[pos]) | unsigned(view[pos+1])<<8 | unsigned(view[pos+2])<<16 | unsigned(view[pos+3])<<24;
    };
    const auto item = tree ? 9 : 166;
    const auto table = tree ? 5 : 205;
    const auto at = word32(data,4+4*item);
    const auto record = data.subspan(at,word32(data,8+4*item)-at);
    MapImage map{256,256,std::vector<std::uint8_t>(256*256*4)};
    const std::array<std::uint8_t,512> palette{};
    SpriteCoverage body;
    drawIndexedFrame(map,128,128,record.subspan(word32(record,table)),palette,&body);
    const auto shadow = readShadowCoverage(128,128,record.subspan(word32(record,table+8)));
    SceneryDepth depth(map.width,map.height);
    depth.addShadow(shadow);
    depth.addBody(body,128);
    int behind = 0, equal = 0, front = 0, shadowOnly = 0, revealedBehind = 0, revealedEqual = 0;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            behind += depth.occludes(x,y,127);
            equal += depth.occludes(x,y,128);
            front += depth.occludes(x,y,129);
            shadowOnly += shadow.covers(x,y) && !body.covers(x,y);
            if (body.covers(x,y)) {
                revealedBehind += !depth.occludes(x,y,127,true);
                revealedEqual += !depth.occludes(x,y,128,true);
            }
        }
    }
    if (behind != (tree ? 7799 : 1273) || equal != (tree ? 7491 : 1176) ||
        front != 0 || shadowOnly != (tree ? 4346 : 444) || (tree && (revealedBehind != 3900 || revealedEqual != 4065)))
        throw std::runtime_error("Scenery depth differs from the Gold renderer");
}
}

int main(int argc, char** argv) try {
    if (argc != 2) return 1;
    checkAssetDepth(argv[1],false);
    checkAssetDepth(argv[1],true);
    std::vector<Placement> units{{"dofficer",60,62,0,100,90,0,0}, {"dguner",64,60,0,100,90,0,0}};
    InfantryScene original(ground(), argv[1], units, true);
    for (int resolution : {1, 2}) {
        checkStandDepth(argv[1], resolution);
        MapImage hd;
        if (resolution == 2) hd = upscalePatch2x(original.compose());
        InfantryScene enlarged(ground(), argv[1], units, false, std::move(hd));
        for (size_t i = 0; i < units.size(); ++i) {
            const auto& before = original.units()[i];
            const auto& after = enlarged.units()[i];
            if (before.id != units[i].id || after.id != units[i].id)
                throw std::runtime_error("Rendering changed the opening live-list order");
            if (after.x != before.x || after.y != before.y || after.height <= before.height ||
                after.width <= before.width) throw std::runtime_error("Incorrect anchor or size");
        }
        auto pixels = enlarged.compose();
        const auto underlay = resolution == 2 ? upscalePatch2x(ground()) : ground();
        if (pixels.rgba == underlay.rgba) throw std::runtime_error("Missing standing poses");
        apply(pixels, enlarged.update(0, 109, 78, 6, 112, true));
        apply(pixels, enlarged.update(1, 118, 80, 7, 218, true));
        if (pixels.rgba != enlarged.compose().rgba) throw std::runtime_error("Walking patches differ from full redraw");
        for (unsigned y = 0; y < pixels.height; ++y) {
            const size_t row = y*size_t(pixels.width)*4;
            if (!std::equal(pixels.rgba.begin()+row, pixels.rgba.begin()+row+80*resolution*4,
                            underlay.rgba.begin()+row)) throw std::runtime_error("Stale soldier at spawn");
        }
        // Hidden (visible=0) and dead (hp=0) units must contribute zero pixels (closes #281 gaps b, c).
        apply(pixels, enlarged.update(0, 109, 78, 6, 112, true, 100, 0));
        apply(pixels, enlarged.update(1, 118, 80, 7, 218, true, 0, 1));
        if (pixels.rgba != underlay.rgba || enlarged.compose().rgba != underlay.rgba)
            throw std::runtime_error("Hidden or dead soldier still painted");
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
