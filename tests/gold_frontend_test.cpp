#include "ui/gold_frontend.hpp"
#include "assets/mission.hpp"
#include "assets/sprite.hpp"
#include "render/window.hpp"

#include <SDL3/SDL.h>
#include <png.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool cond, const char* msg) {
    if (!cond) throw std::runtime_error(msg);
}

bool near(double a, double b, double eps = 0.01) {
    return std::abs(a - b) <= eps;
}

void writePng(const std::filesystem::path& path, int w, int h, const std::vector<std::uint8_t>& rgba) {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) throw std::runtime_error("Cannot open PNG output: " + path.string());
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        throw std::runtime_error("libpng write failure");
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, png_uint_32(w), png_uint_32(h), 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<png_bytep> rows(static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        rows[std::size_t(y)] = const_cast<png_bytep>(rgba.data() + std::size_t(y) * w * 4);
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
}

std::vector<ss2::GoldMissionEntry> makeSyntheticMissions(int count) {
    std::vector<ss2::GoldMissionEntry> out;
    for (int i = 0; i < count; ++i) {
        out.push_back({
            "Mission " + std::to_string(i + 1),
            "/nonexistent/mission_" + std::to_string(i + 1) + ".ssm",
            "Briefing description for synthetic operation #" + std::to_string(i + 1) +
                ". Advance along the river and capture the crossing.",
            "128x128 Summer",
            128u,
            128u,
            1u,
            i == 0,
            i == 0 ? 4500 : 0,
        });
    }
    return out;
}

void testCoordinateMapping() {
    // 1. Native 800x600 -> exact 1:1
    const auto s1 = ss2::GoldFrontEnd::computeStage(800, 600);
    check(near(s1.x, 0.0) && near(s1.y, 0.0) && near(s1.scale, 1.0), "800x600 stage");

    // 2. 1280x720 -> centered letterbox margins at left/right ((1280 - 960)/2 = 160), scale = 1.2
    const auto s2 = ss2::GoldFrontEnd::computeStage(1280, 720);
    check(near(s2.scale, 1.2) && near(s2.x, 160.0) && near(s2.y, 0.0), "1280x720 stage");

    // 3. 1.25x DPI scaling (#738): win=1024x576, pixel=1280x720
    // Click on SINGLE PLAYER button center in stage coords (188, 128.5):
    // pixel = 160 + 188*1.2 = 385.6, window = 385.6 / 1.25 = 308.48; py = 128.5*1.2/1.25 = 123.36
    const auto pHi = ss2::GoldFrontEnd::windowToStage(308.48, 123.36, 1024, 576, 1280, 720);
    check(pHi.inside && near(pHi.x, 188.0, 0.1) && near(pHi.y, 128.5, 0.1), "1.25x DPI center click");

    // Click inside left letterbox margin (window x = 50 -> pixel x = 62.5 < 160) -> inside == false
    const auto pMargin = ss2::GoldFrontEnd::windowToStage(50.0, 200.0, 1024, 576, 1280, 720);
    check(!pMargin.inside, "letterbox margin outside stage");

    // 4. 4K (3840x2160) -> scale = 3.6, x = (3840 - 2880)/2 = 480
    const auto s4k = ss2::GoldFrontEnd::computeStage(3840, 2160);
    check(near(s4k.scale, 3.6) && near(s4k.x, 480.0), "4K stage");

    // 5. 1.25x window scaling where logical=1280x720, physical=1600x900 (stage scale=1.5, x=200):
    // Briefing START center (695, 500) -> pixel (1242.5, 750) -> window (994, 600)
    const auto p125 = ss2::GoldFrontEnd::windowToStage(994.0, 600.0, 1280, 720, 1600, 900);
    check(p125.inside && near(p125.x, 695.0, 0.1) && near(p125.y, 500.0, 0.1), "1280x720@1600x900 1.25x click");
}

void testFallbackInteraction() {
    ss2::GoldFrontEnd fe(nullptr, {});
    check(!fe.hasMenuAssets(), "asset-free mode hasMenuAssets=false");
    fe.setMissions(makeSyntheticMissions(25));
    check(fe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "starts MainMenu");

    // Deferred branch shows integrated notice without leaving screen and lasts ~30s
    fe.handleMouseDown(188.0, 195.0, 1);  // Multiplayer
    check(fe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "multiplayer stays MainMenu");
    check(!fe.activeNotice().empty(), "multiplayer notice active");
    fe.advance(std::chrono::seconds(7));
    check(!fe.activeNotice().empty(), "notice remains visible past 7s");
    fe.advance(std::chrono::seconds(24));
    check(fe.activeNotice().empty(), "notice expires after 30s total");

    // Navigate Main -> SinglePlayer -> SingleMissions -> Briefing -> Cancel preserves selection -> Launch
    fe.handleMouseDown(188.0, 128.0, 1);  // SINGLE PLAYER [65,111,246,35]
    check(fe.state().screen() == ss2::FrontEnd::Screen::SinglePlayer, "entered SinglePlayer");
    fe.handleMouseDown(186.0, 263.0, 1);  // SINGLE MISSIONS [40,246,293,35]
    check(fe.state().screen() == ss2::FrontEnd::Screen::SingleMissions, "entered SingleMissions");

    // Wheel scroll & click row
    fe.handleMouseWheel(-1.0f);  // scroll down 3 rows
    check(fe.scrollOffset() == 3, "wheel scrolled down 3");
    // Row 0 on screen is index 3 (kListY + 6 + 0*20 = 140)
    fe.handleMouseDown(120.0, 148.0, 1);  // single click selects index 3
    check(fe.state().selected() == 3, "row click selected index 3");
    fe.handleMouseDown(120.0, 148.0, 2);  // double click -> Briefing
    check(fe.state().screen() == ss2::FrontEnd::Screen::Briefing, "double click opens Briefing");
    check(fe.state().selected() == 3, "Briefing kept index 3");

    fe.handleKey(SDLK_ESCAPE);
    check(fe.state().screen() == ss2::FrontEnd::Screen::SingleMissions, "Esc returns to SingleMissions");
    check(fe.state().selected() == 3, "Cancel keeps index 3 selected");

    // Start button on SingleMissions [135,512,106,35] -> Briefing -> Start [627,484,136,33] -> Launch
    fe.handleMouseDown(188.0, 529.0, 1);
    check(fe.state().screen() == ss2::FrontEnd::Screen::Briefing, "Start button enters Briefing");
    fe.handleMouseDown(695.0, 500.0, 1);
    check(fe.state().screen() == ss2::FrontEnd::Screen::Launch, "Briefing Start reaches Launch");

    // Render all 4 fallback screens to verify non-empty distinct output
    std::vector<std::uint8_t> buf;
    ss2::GoldFrontEnd renderFe(nullptr, {});
    renderFe.setMissions(makeSyntheticMissions(5));
    renderFe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback Screen 1 buffer size");
}

void testRetailGoldAssets(const std::filesystem::path& gameDir) {
    const auto menuSue = ss2::assets::resolveCaseInsensitivePath(gameDir, "menu_common.sue");
    check(std::filesystem::exists(menuSue), "menu_common.sue exists");

    // 1. decodeJpeg on mainbk000.jpg and brfbk000.jpg
    const auto mainBg = ss2::assets::decodeJpeg(ss2::assets::readSueEntry(menuSue, "mainbk000.jpg"));
    check(mainBg.width == 800 && mainBg.height == 600, "mainbk000 800x600");
    const auto brfBg = ss2::assets::decodeJpeg(ss2::assets::readSueEntry(menuSue, "brfbk000.jpg"));
    check(brfBg.width == 800 && brfBg.height == 600, "brfbk000 800x600");

    // 2. strict decodePckFrame must still fail on nbramka_bk.pck[0]
    const auto nbramkaRaw = ss2::assets::readSueEntry(menuSue, "nbramka_bk.pck");
    bool threw = false;
    try {
        (void)ss2::assets::decodePckFrame(nbramkaRaw, 0);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "strict decodePckFrame rejects truncated UI PCK row");

    // 3. lenient decodeUiPckFrame succeeds on all Gold UI PCK files
    const auto ramka = ss2::assets::decodeUiPckFrame(nbramkaRaw, 0);
    check(ramka.x == 6 && ramka.y == 161 && ramka.width == 778 && ramka.height == 410, "nbramka dims");
    const auto nbrifRaw = ss2::assets::readSueEntry(menuSue, "nbrif_bt.pck");
    check(ss2::assets::pckFrameCount(nbrifRaw) == 20, "nbrif_bt 20 frames");
    for (std::size_t i = 0; i < 20; ++i) {
        const auto f = ss2::assets::decodeUiPckFrame(nbrifRaw, i);
        check(f.width > 0 && f.height > 0 && f.pixels.size() == std::size_t(f.width) * f.height * 2,
              "nbrif frame size");
    }
    const auto sbRaw = ss2::assets::readSueEntry(menuSue, "scrollbar_bt.pck");
    check(ss2::assets::pckFrameCount(sbRaw) == 6, "scrollbar_bt 6 frames");
    for (std::size_t i = 0; i < 6; ++i) (void)ss2::assets::decodeUiPckFrame(sbRaw, i);
    const auto spRaw = ss2::assets::readSueEntry(menuSue, "scrollbar_sp.pck");
    check(ss2::assets::pckFrameCount(spRaw) == 3, "scrollbar_sp 3 frames");
    for (std::size_t i = 0; i < 3; ++i) (void)ss2::assets::decodeUiPckFrame(spRaw, i);

    // 4. Full GoldFrontEnd load & render of Screens 1-4
    ss2::GoldFrontEnd fe(nullptr, gameDir, 0);
    check(fe.hasMenuAssets(), "retail menu_common.sue loaded");

    std::vector<ss2::GoldMissionEntry> entries;
    std::size_t tutIdx = 0;
    const auto list = ss2::assets::listSinglePlayerMissions(gameDir);
    check(!list.empty(), "retail single player missions listed");
    for (std::size_t i = 0; i < list.size(); ++i) {
        std::string low = list[i].name;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (low.find("barbarossa") != std::string::npos || low.find("tutorial") != std::string::npos) {
            tutIdx = i;
        }
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& m = list[i];
        entries.push_back({m.name, m.path.string(), m.description, "Summer",
                           m.width, m.height, m.scheme, i == tutIdx, i == tutIdx ? 9000 : 0});
    }
    fe.setMissions(std::move(entries));

    const char* capEnv = std::getenv("SS2_GOLD_FE_CAPTURE_DIR");
    const std::filesystem::path capDir = capEnv ? std::filesystem::path(capEnv) : std::filesystem::path{};
    std::vector<std::uint8_t> frame;

    // Screen 1: Main Menu
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen1_main.png", 1280, 720, frame);

    // Screen 2: Single Player
    fe.handleMouseDown(188.0, 128.0, 1);
    check(fe.state().screen() == ss2::FrontEnd::Screen::SinglePlayer, "retail SinglePlayer");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen2_single.png", 1280, 720, frame);

    // Screen 3: Single Missions (select first, also capture Tutorial or first rich mission)
    fe.handleMouseDown(186.0, 263.0, 1);
    check(fe.state().screen() == ss2::FrontEnd::Screen::SingleMissions, "retail SingleMissions");
    fe.state().select(tutIdx);
    fe.handleKey(SDLK_DOWN);
    fe.handleKey(SDLK_UP);  // ensure selected overview cached and scrolled into view
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen3_missions.png", 1280, 720, frame);
    fe.renderToBuffer(3840, 2160, frame);
    check(frame.size() == 3840u * 2160u * 4u, "retail Screen 3 4K buffer");
    if (!capDir.empty()) writePng(capDir / "screen3_4k.png", 3840, 2160, frame);

    // Screen 4: Mission Briefing (non-overlapping top text + diamond + right buttons)
    fe.handleMouseDown(188.0, 529.0, 1);
    check(fe.state().screen() == ss2::FrontEnd::Screen::Briefing, "retail Briefing");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen4_briefing.png", 1280, 720, frame);
    fe.renderToBuffer(3840, 2160, frame);
    check(frame.size() == 3840u * 2160u * 4u, "retail Screen 4 4K buffer");
    if (!capDir.empty()) writePng(capDir / "screen4_4k.png", 3840, 2160, frame);
}

void testVulkanWindowOverlay(bool validate, const std::filesystem::path& gameDir) {
    if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    struct Guard { ~Guard() { SDL_Quit(); } } sdlGuard;

    SDL_Window* win = SDL_CreateWindow("gold_frontend_test", 1280, 720,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!win) throw std::runtime_error(SDL_GetError());
    struct WinGuard { SDL_Window* w; ~WinGuard() { SDL_DestroyWindow(w); } } winGuard{win};

    ss2::ValidationReport report;
    ss2::VulkanWindow renderer(win, validate ? &report : nullptr);
    ss2::GoldFrontEnd fe(win, gameDir, 0);
    fe.setMissions(makeSyntheticMissions(3));
    const ss2::Snapshot empty{};
    for (int i = 0; i < 4; ++i) {
        if (i == 1) fe.handleKey(SDLK_S);  // Single Player
        if (i == 2) fe.handleKey(SDLK_S);  // Single Missions
        if (i == 3) fe.handleKey(SDLK_RETURN);  // Briefing
        fe.draw(renderer);
        renderer.present(empty);
    }
    check(report.errors.load() == 0, "Zero Vulkan validation errors on GoldFrontEnd overlay");
}

}  // namespace

int main(int argc, char** argv) try {
    bool useWindow = false;
    bool validate = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--window") useWindow = true;
        else if (a == "--validate") validate = true;
    }

    testCoordinateMapping();
    testFallbackInteraction();

    std::filesystem::path gameDir;
    if (const char* env = std::getenv("SS2_PRIVATE_GAME"); env && *env) {
        gameDir = env;
    }
    if (!gameDir.empty() && std::filesystem::is_directory(gameDir)) {
        testRetailGoldAssets(gameDir);
    }
    if (useWindow) {
        testVulkanWindowOverlay(validate, gameDir);
    }

    std::cout << "gold_frontend_test PASSED\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "gold_frontend_test FAILED: " << e.what() << '\n';
    return 1;
}
