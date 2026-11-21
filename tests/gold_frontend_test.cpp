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

std::vector<ss2::GoldCampaignEntry> makeSyntheticCampaigns() {
    std::vector<ss2::GoldCampaignEntry> out;
    ss2::GoldCampaignEntry german{"German Campaign", {}};
    for (int i = 0; i < 8; ++i) {
        const std::string code = "02300" + std::to_string(i);
        german.missions.push_back({
            code,
            std::to_string(i + 1) + ". Operation #" + std::to_string(i + 1),
            "/nonexistent/campaign/" + code + ".ssc",
            "/nonexistent/campaign/EASY/" + code + ".ssc",
            "Synthetic campaign briefing #" + std::to_string(i + 1) + ".",
            "256x256 Summer",
            256u,
            256u,
            1u,
            i == 0,
            i == 0 ? 7200 : 0,
        });
    }
    out.push_back(std::move(german));
    out.push_back({"Japanese Campaign", {
        {"002000", "1. Omen of Victory", "/nonexistent/campaign/002000.ssc",
         "/nonexistent/campaign/EASY/002000.ssc", "Island landing.", "256x256 Beach", 256u, 256u, 2u, false, 0},
    }});
    return out;
}

void testCoordinateMapping() {
    const auto s1 = ss2::GoldFrontEnd::computeStage(800, 600);
    check(near(s1.x, 0.0) && near(s1.y, 0.0) && near(s1.scale, 1.0), "800x600 stage");

    const auto s2 = ss2::GoldFrontEnd::computeStage(1280, 720);
    check(near(s2.scale, 1.2) && near(s2.x, 160.0) && near(s2.y, 0.0), "1280x720 stage");

    const auto pHi = ss2::GoldFrontEnd::windowToStage(308.48, 123.36, 1024, 576, 1280, 720);
    check(pHi.inside && near(pHi.x, 188.0, 0.1) && near(pHi.y, 128.5, 0.1), "1.25x DPI center click");

    const auto pMargin = ss2::GoldFrontEnd::windowToStage(50.0, 200.0, 1024, 576, 1280, 720);
    check(!pMargin.inside, "letterbox margin outside stage");

    const auto s4k = ss2::GoldFrontEnd::computeStage(3840, 2160);
    check(near(s4k.scale, 3.6) && near(s4k.x, 480.0), "4K stage");

    const auto p125 = ss2::GoldFrontEnd::windowToStage(994.0, 600.0, 1280, 720, 1600, 900);
    check(p125.inside && near(p125.x, 695.0, 0.1) && near(p125.y, 500.0, 0.1), "1280x720@1600x900 1.25x click");
}

void testFallbackInteraction() {
    ss2::GoldFrontEnd fe(nullptr, {});
    check(!fe.hasMenuAssets(), "asset-free mode hasMenuAssets=false");
    fe.setMissions(makeSyntheticMissions(25));
    fe.setCampaigns(makeSyntheticCampaigns());
    check(fe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "starts MainMenu");

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

    fe.handleMouseWheel(-1.0f);  // scroll down 3 rows
    check(fe.scrollOffset() == 3, "wheel scrolled down 3");
    fe.handleMouseDown(120.0, 148.0, 1);  // single click selects index 3
    check(fe.state().selected() == 3, "row click selected index 3");
    fe.handleMouseDown(120.0, 148.0, 2);  // double click -> Briefing
    check(fe.state().screen() == ss2::FrontEnd::Screen::Briefing, "double click opens Briefing");
    check(fe.state().selected() == 3, "Briefing kept index 3");

    fe.handleKey(SDLK_ESCAPE);
    check(fe.state().screen() == ss2::FrontEnd::Screen::SingleMissions, "Esc returns to SingleMissions");
    check(fe.state().selected() == 3, "Cancel keeps index 3 selected");

    fe.handleMouseDown(188.0, 529.0, 1);  // Start on SingleMissions
    check(fe.state().screen() == ss2::FrontEnd::Screen::Briefing, "Start button enters Briefing");
    fe.handleMouseDown(695.0, 500.0, 1);  // Start on Briefing
    check(fe.state().screen() == ss2::FrontEnd::Screen::Launch, "Briefing Start reaches Launch");

    // Synthetic Campaigns navigation: SinglePlayer -> Campaigns -> German -> Mission 2 -> Difficulty Easy -> Launch
    ss2::GoldFrontEnd cfe(nullptr, {});
    cfe.setCampaigns(makeSyntheticCampaigns());
    std::vector<std::uint8_t> buf;
    cfe.handleMouseDown(188.0, 128.0, 1);  // SinglePlayer
    cfe.handleMouseDown(187.0, 196.0, 1);  // Campaigns [78,179,218,35]
    check(cfe.state().screen() == ss2::FrontEnd::Screen::Campaigns, "clicked Campaigns");
    cfe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback Campaigns buffer size");

    cfe.handleMouseDown(188.0, 128.0, 1);  // Germany button [78,111,220,35]
    check(cfe.state().screen() == ss2::FrontEnd::Screen::CampaignMissions, "clicked Germany -> CampaignMissions");
    cfe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback CampaignMissions buffer size");

    cfe.handleMouseDown(120.0, 168.0, 1);  // Row 1 (kListY+6+20 = 160..180)
    check(cfe.state().selectedCampaignMission() == 1, "clicked campaign row 1");
    // Toggle difficulty: Normal(2) -> Hard(3) -> Easy(1) via Difficulty button [268,515,244,30]
    cfe.handleMouseDown(390.0, 530.0, 1);
    check(cfe.difficulty() == 3, "Difficulty click -> Hard (3)");
    cfe.handleMouseDown(390.0, 530.0, 1);
    check(cfe.difficulty() == 1, "Difficulty click -> Easy (1)");
    check(cfe.state().launchPath() == "/nonexistent/campaign/EASY/023001.ssc",
          "Campaign Easy selection routes to EASY/*.ssc");

    cfe.handleMouseDown(120.0, 168.0, 2);  // double click -> Briefing
    check(cfe.state().screen() == ss2::FrontEnd::Screen::Briefing, "double click opens campaign Briefing");
    cfe.handleMouseDown(695.0, 542.0, 1);  // Cancel on Briefing [627,526,136,33]
    check(cfe.state().screen() == ss2::FrontEnd::Screen::CampaignMissions &&
          cfe.state().selectedCampaignMission() == 1,
          "Cancel from campaign Briefing preserves mission index");
    cfe.handleMouseDown(188.0, 529.0, 1);  // Start -> Briefing
    check(cfe.state().screen() == ss2::FrontEnd::Screen::Briefing, "Start reopens campaign Briefing");
    cfe.handleMouseDown(695.0, 500.0, 1);  // Briefing Start -> Launch
    check(cfe.state().screen() == ss2::FrontEnd::Screen::Launch &&
          cfe.state().launchPath() == "/nonexistent/campaign/EASY/023001.ssc",
          "Campaign Briefing launches EASY mission path");
}

void testRetailGoldAssets(const std::filesystem::path& gameDir) {
    const auto menuSue = ss2::assets::resolveCaseInsensitivePath(gameDir, "menu_common.sue");
    check(std::filesystem::exists(menuSue), "menu_common.sue exists");

    const auto mainBg = ss2::assets::decodeJpeg(ss2::assets::readSueEntry(menuSue, "mainbk000.jpg"));
    check(mainBg.width == 800 && mainBg.height == 600, "mainbk000 800x600");
    const auto brfBg = ss2::assets::decodeJpeg(ss2::assets::readSueEntry(menuSue, "brfbk000.jpg"));
    check(brfBg.width == 800 && brfBg.height == 600, "brfbk000 800x600");

    const auto nbramkaRaw = ss2::assets::readSueEntry(menuSue, "nbramka_bk.pck");
    bool threw = false;
    try {
        (void)ss2::assets::decodePckFrame(nbramkaRaw, 0);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "strict decodePckFrame rejects truncated UI PCK row");

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

    // Load all 5 retail campaigns (31 missions)
    const auto campList = ss2::assets::listCampaigns(gameDir);
    check(campList.size() == 5, "retail 5 campaigns listed");
    std::size_t totalCampMissions = 0;
    std::vector<ss2::GoldCampaignEntry> campEntries;
    for (std::size_t ci = 0; ci < campList.size(); ++ci) {
        const auto& c = campList[ci];
        ss2::GoldCampaignEntry ce;
        ce.name = c.name;
        for (std::size_t mi = 0; mi < c.missions.size(); ++mi) {
            const auto& m = c.missions[mi];
            ++totalCampMissions;
            check(!m.description.empty(), "retail campaign mission has non-empty I-DESC");
            check(!m.easyPath.empty() && std::filesystem::is_regular_file(m.easyPath),
                  "retail campaign mission has EASY path");
            ce.missions.push_back({m.code, m.title, m.path.string(), m.easyPath.string(),
                                   m.description, "Summer", m.width, m.height, m.scheme,
                                   ci == 0 && mi == 0, (ci == 0 && mi == 0) ? 8500 : 0});
        }
        campEntries.push_back(std::move(ce));
    }
    check(totalCampMissions == 31, "retail campaigns total 31 missions");
    fe.setCampaigns(std::move(campEntries));

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

    // Screen 2b: Campaigns selector + dossier
    fe.handleMouseDown(187.0, 196.0, 1);  // CAMPAIGNS button [78,179,218,35]
    check(fe.state().screen() == ss2::FrontEnd::Screen::Campaigns, "retail Campaigns");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen2b_campaigns.png", 1280, 720, frame);

    // Screen 3b: German Campaign missions + Difficulty switch (Normal -> Hard -> Easy)
    fe.handleKey(SDLK_G);
    check(fe.state().screen() == ss2::FrontEnd::Screen::CampaignMissions &&
          fe.state().selectedCampaign() == 0, "G opens German Campaign missions");
    check(fe.state().launchPath() == campList[0].missions[0].path.string(), "Normal launch path");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen3b_campaign_missions.png", 1280, 720, frame);
    fe.renderToBuffer(3840, 2160, frame);
    check(frame.size() == 3840u * 2160u * 4u, "retail Screen 3b Campaign 4K buffer");
    if (!capDir.empty()) writePng(capDir / "screen3b_campaign_4k.png", 3840, 2160, frame);

    fe.handleKey(SDLK_D);  // Normal -> Hard
    check(fe.difficulty() == 3, "D toggles to Hard");
    fe.handleKey(SDLK_D);  // Hard -> Easy
    check(fe.difficulty() == 1, "D toggles to Easy");
    check(fe.state().launchPath() == campList[0].missions[0].easyPath.string(),
          "Easy routes launchPath to EASY/023000.ssc");

    // Screen 4b: Campaign Briefing
    fe.handleMouseDown(188.0, 529.0, 1);  // Start -> Briefing
    check(fe.state().screen() == ss2::FrontEnd::Screen::Briefing &&
          fe.state().briefingFromCampaign(), "retail Campaign Briefing");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen4b_campaign_briefing.png", 1280, 720, frame);

    // Walk back up to Single Player -> Single Missions -> Tutorial Briefing
    fe.handleKey(SDLK_ESCAPE);  // Briefing -> CampaignMissions
    fe.handleKey(SDLK_ESCAPE);  // CampaignMissions -> Campaigns
    fe.handleKey(SDLK_ESCAPE);  // Campaigns -> SinglePlayer
    check(fe.state().screen() == ss2::FrontEnd::Screen::SinglePlayer, "Esc returns to SinglePlayer");

    // Screen 3: Single Missions
    fe.handleMouseDown(186.0, 263.0, 1);
    check(fe.state().screen() == ss2::FrontEnd::Screen::SingleMissions, "retail SingleMissions");
    fe.state().select(tutIdx);
    fe.handleKey(SDLK_DOWN);
    fe.handleKey(SDLK_UP);
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen3_missions.png", 1280, 720, frame);
    fe.renderToBuffer(3840, 2160, frame);
    check(frame.size() == 3840u * 2160u * 4u, "retail Screen 3 4K buffer");
    if (!capDir.empty()) writePng(capDir / "screen3_4k.png", 3840, 2160, frame);

    // Screen 4: Mission Briefing
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
    fe.setCampaigns(makeSyntheticCampaigns());
    const ss2::Snapshot empty{};
    for (int i = 0; i < 6; ++i) {
        if (i == 1) fe.handleKey(SDLK_S);       // Single Player
        if (i == 2) fe.handleKey(SDLK_C);       // Campaigns
        if (i == 3) fe.handleKey(SDLK_G);       // German Campaign missions
        if (i == 4) fe.handleKey(SDLK_D);       // Cycle difficulty
        if (i == 5) fe.handleKey(SDLK_RETURN);  // Campaign Briefing
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
