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
    check(fe.state().screen() == ss2::FrontEnd::Screen::Multiplayer, "multiplayer enters Screen::Multiplayer");
    fe.handleMouseDown(188.0, 529.0, 1);  // Start on empty Host MP list
    check(fe.state().screen() == ss2::FrontEnd::Screen::Multiplayer, "empty MP Start stays Multiplayer");
    check(!fe.activeNotice().empty(), "empty MP Start notice active");
    fe.advance(std::chrono::seconds(7));
    check(!fe.activeNotice().empty(), "notice remains visible past 7s");
    fe.advance(std::chrono::seconds(24));
    check(fe.activeNotice().empty(), "notice expires after 30s total");

    // Toggle Host/Join, edit Host Address & Port, select MP mission, render, then return to MainMenu
    std::vector<std::uint8_t> mpBuf;
    fe.handleMouseDown(600.0, 153.0, 1);  // mp_mode
    check(!fe.state().mpIsHost(), "mp_mode clicked -> Join Match");
    fe.handleMouseDown(500.0, 220.0, 1);  // mp_host_field
    for (int i = 0; i < 9; ++i) fe.handleKey(SDLK_BACKSPACE);
    {
        SDL_Event te{};
        te.type = SDL_EVENT_TEXT_INPUT;
        te.text.text = "10.0.0.5";
        fe.handleEvent(te);
    }
    check(fe.state().mpHostAddress() == "10.0.0.5", "edited Host Address in Multiplayer screen");
    fe.handleMouseDown(500.0, 284.0, 1);  // mp_port_field
    for (int i = 0; i < 5; ++i) fe.handleKey(SDLK_BACKSPACE);
    {
        SDL_Event pe{};
        pe.type = SDL_EVENT_TEXT_INPUT;
        pe.text.text = "28000";
        fe.handleEvent(pe);
    }
    fe.handleMouseDown(600.0, 153.0, 1);  // commit port and toggle back to Host Match
    check(fe.state().mpIsHost() && fe.state().mpPort() == 28000u, "edited Port committed in Multiplayer screen");
    check(fe.state().mpNation() == 1u, "Gold Multiplayer defaults to German (1)");
    fe.handleMouseDown(688.0, 284.0, 1);  // mp_nation [608,268,160,32]
    check(fe.state().mpNation() == 2u, "clicked mp_nation cycles nation");
    fe.handleMouseDown(500.0, 322.0, 1);  // mp_players [438,308,160,28]: 2 -> 3
    check(fe.state().mpPlayerCount() == 3u, "clicked mp_players cycles 2 -> 3");
    fe.handleMouseDown(500.0, 322.0, 1);  // mp_players: 3 -> 4
    check(fe.state().mpPlayerCount() == 4u && !fe.state().mpTeam2v2(), "clicked mp_players cycles 3 -> 4 (FFA)");
    fe.handleMouseDown(688.0, 322.0, 1);  // mp_teams [608,308,160,28]: FFA -> 2v2
    check(fe.state().mpTeam2v2(), "clicked mp_teams toggles 4P FFA -> 2v2");
    fe.handleMouseDown(500.0, 322.0, 1);  // mp_players: 4 -> 2
    check(fe.state().mpPlayerCount() == 2u, "clicked mp_players cycles 4 -> 2");
    fe.setMultiplayerMissions(makeSyntheticMissions(5));
    fe.handleMouseDown(120.0, 168.0, 1);  // row 1
    check(fe.state().selectedMultiplayerMission() == 1, "clicked MP map row 1");
    fe.renderToBuffer(1280, 720, mpBuf);
    check(mpBuf.size() == 1280u * 720u * 4u, "fallback Multiplayer buffer size");
    fe.handleKey(SDLK_ESCAPE);
    check(fe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "Esc from Multiplayer returns to MainMenu");

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

    // Empty Custom Campaigns -> Cancel -> populated Custom Campaigns -> ccampaign:0 -> Esc back.
    cfe.handleMouseDown(188.0, 529.0, 1);  // Custom [118,512,140,35]
    check(cfe.state().screen() == ss2::FrontEnd::Screen::CustomCampaigns, "clicked Custom -> CustomCampaigns");
    cfe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback empty CustomCampaigns buffer size");
    cfe.handleMouseDown(600.0, 529.0, 1);  // Cancel [542,512,118,35]
    check(cfe.state().screen() == ss2::FrontEnd::Screen::Campaigns, "Cancel from CustomCampaigns returns to Campaigns");

    cfe.setCustomCampaigns({
        {"Operation SeaLion", {
            {"01_dover", "1. Dover Crossing", "/nonexistent/custom/01_dover.ssc",
             "/nonexistent/custom/EASY/01_dover.ssc", "Cross the channel.", "256x256 Summer", 256u, 256u, 1u, false, 0},
        }},
    });
    cfe.handleMouseDown(188.0, 529.0, 1);  // Custom
    check(cfe.state().screen() == ss2::FrontEnd::Screen::CustomCampaigns, "re-entered CustomCampaigns");
    cfe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback populated CustomCampaigns buffer size");
    cfe.handleMouseDown(188.0, 128.0, 1);  // ccampaign:0 [78,111,220,35]
    check(cfe.state().screen() == ss2::FrontEnd::Screen::CampaignMissions && cfe.state().inCustomCampaign(),
          "clicked ccampaign:0 -> custom CampaignMissions");
    cfe.handleKey(SDLK_ESCAPE);
    check(cfe.state().screen() == ss2::FrontEnd::Screen::CustomCampaigns, "Esc returns to CustomCampaigns");
    cfe.handleKey(SDLK_ESCAPE);
    check(cfe.state().screen() == ss2::FrontEnd::Screen::Campaigns, "Esc returns from CustomCampaigns to Campaigns");

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

    // Synthetic SinglePlayer multi-profile flow: last-profile guard, Esc cancel edit, New, Rename, Switch, Delete.
    {
        ss2::GoldFrontEnd pfe(nullptr, {});
        pfe.handleMouseDown(188.0, 128.0, 1);  // SinglePlayer
        check(pfe.state().screen() == ss2::FrontEnd::Screen::SinglePlayer &&
              pfe.settings().activeProfile == "YourID" && pfe.settings().profiles.size() == 1,
              "SinglePlayer starts with default YourID profile");
        pfe.handleMouseDown(521.0, 475.0, 1);  // profile_delete [480,462,82,27]
        check(pfe.settings().profiles.size() == 1 && !pfe.activeNotice().empty(),
              "Delete on last profile keeps YourID and shows notice");
        pfe.handleMouseDown(333.0, 110.0, 1);  // profile_new [305,97,56,27]
        pfe.handleKey(SDLK_ESCAPE);
        check(pfe.state().screen() == ss2::FrontEnd::Screen::SinglePlayer,
              "Esc while editing new profile cancels edit without leaving SinglePlayer");

        pfe.handleMouseDown(333.0, 110.0, 1);  // profile_new
        {
            SDL_Event te{};
            te.type = SDL_EVENT_TEXT_INPUT;
            te.text.text = "Rommel";
            pfe.handleEvent(te);
        }
        pfe.handleKey(SDLK_RETURN);
        check(pfe.settings().activeProfile == "Rommel" && pfe.settings().profiles.size() == 2 &&
              pfe.takeSettingsSaved(),
              "New profile creates Rommel and saves settings");

        pfe.handleMouseDown(118.0, 141.0, 1);  // profile_rename [69,126,98,30]
        for (int b = 0; b < 6; ++b) pfe.handleKey(SDLK_BACKSPACE);
        {
            SDL_Event re{};
            re.type = SDL_EVENT_TEXT_INPUT;
            re.text.text = "Manstein";
            pfe.handleEvent(re);
        }
        pfe.handleKey(SDLK_RETURN);
        check(pfe.settings().activeProfile == "Manstein" && pfe.takeSettingsSaved(),
              "Rename updates active profile to Manstein");

        pfe.handleMouseDown(550.0, 111.0, 1);  // profile_row:0 [466,101..121]
        check(pfe.settings().activeProfile == "YourID" && pfe.takeSettingsSaved(),
              "Clicking profile_row:0 switches back to YourID");
        pfe.handleMouseDown(550.0, 131.0, 1);  // profile_row:1 [466,121..141]
        check(pfe.settings().activeProfile == "Manstein" && pfe.takeSettingsSaved(),
              "Clicking profile_row:1 selects Manstein");
        pfe.renderToBuffer(1280, 720, buf);
        check(buf.size() == 1280u * 720u * 4u, "fallback SinglePlayer profile buffer size");
        pfe.handleMouseDown(521.0, 475.0, 1);  // profile_delete
        check(pfe.settings().profiles.size() == 1 && pfe.settings().activeProfile == "YourID" &&
              pfe.takeSettingsSaved(),
              "Deleting Manstein restores YourID");
    }

    // Synthetic Ratings + Credits flow (tabs, scrolling, auto-scroll, pause/resume, Esc/Cancel).
    ss2::GoldFrontEnd rfe(nullptr, {});
    rfe.setMissions(makeSyntheticMissions(25));
    rfe.setCampaigns(makeSyntheticCampaigns());
    rfe.handleMouseDown(185.0, 329.0, 1);  // RATINGS [115,312,140,35]
    check(rfe.state().screen() == ss2::FrontEnd::Screen::Ratings, "clicked Ratings -> Ratings");
    rfe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback Ratings Campaigns buffer size");

    rfe.handleMouseDown(325.0, 108.0, 1);  // Single Missions tab [220,94,210,28]
    check(rfe.state().ratingsMode() == ss2::FrontEnd::RatingsMode::SingleMissions,
          "clicked Single Missions tab in Ratings");
    rfe.handleMouseWheel(-1.0f);
    check(rfe.ratingsScroll() > 0, "Ratings list scrolls down via wheel");
    rfe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback Ratings SingleMissions buffer size");

    rfe.handleKey(SDLK_C);
    check(rfe.state().ratingsMode() == ss2::FrontEnd::RatingsMode::Campaigns &&
          rfe.ratingsScroll() == 0, "C switches Ratings back to Campaigns and resets scroll");
    rfe.handleKey(SDLK_ESCAPE);
    check(rfe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "Esc returns from Ratings to MainMenu");

    rfe.handleMouseDown(185.0, 462.0, 1);  // CREDITS [115,445,140,35]
    check(rfe.state().screen() == ss2::FrontEnd::Screen::Credits, "clicked Credits -> Credits");
    check(rfe.creditsLineCount() > 0, "asset-free Credits has fallback attribution lines");
    rfe.renderToBuffer(1280, 720, buf);
    const double initialScroll = rfe.creditsScroll();
    rfe.advance(std::chrono::milliseconds(500));
    const double advancedScroll = rfe.creditsScroll();
    check(advancedScroll > initialScroll, "Credits auto-scrolls on advance(dt)");
    rfe.handleKey(SDLK_SPACE);
    check(rfe.creditsPaused(), "Space pauses Credits scroll");
    rfe.advance(std::chrono::milliseconds(500));
    check(near(rfe.creditsScroll(), advancedScroll, 1e-6), "Paused Credits holds scroll steady");
    rfe.handleKey(SDLK_SPACE);
    check(!rfe.creditsPaused(), "Second Space resumes Credits scroll");
    rfe.handleMouseDown(600.0, 529.0, 1);  // CANCEL [542,512,118,35]
    check(rfe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "Cancel returns from Credits to MainMenu");

    // Synthetic Options flow: live language preview + Cancel revert, then Skip Intro & Smooth Terrain + OK commit.
    ss2::GoldFrontEnd ofe(nullptr, {});
    ss2::UserSettings initCfg{};
    initCfg.difficulty = 2;
    initCfg.skipIntro = false;
    initCfg.smoothTerrain = false;
    ofe.setSettings(initCfg);
    ofe.handleMouseDown(185.0, 262.0, 1);  // OPTIONS [115,245,141,35]
    check(ofe.state().screen() == ss2::FrontEnd::Screen::Options, "clicked Options -> Options");
    ofe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback Options buffer size");
    // Toggle language row (index 1: y=94+42+4=140..168) -> increments Language::english -> french.
    ofe.handleMouseDown(674.0, 154.0, 1);
    check(ss2::language() == ss2::Language::french, "Options live-previews Language");
    // Toggle Skip Intro row (index 8: y=94+8*42+4=434..462)
    ofe.handleMouseDown(674.0, 448.0, 1);
    ofe.handleKey(SDLK_ESCAPE);
    check(ofe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "Esc returns from Options to MainMenu");
    check(ss2::language() == ss2::Language::english, "Cancel/Esc reverts live Language preview");
    check(!ofe.takeSettingsSaved() && !ofe.settings().skipIntro, "Cancel/Esc discards unsaved Options edits");

    ofe.handleKey(SDLK_O);
    check(ofe.state().screen() == ss2::FrontEnd::Screen::Options, "O reopens Options");
    // Use exact keyboard navigation on Options: start focused on opt_difficulty -> 6x Down (opt_smooth), Right, 2x Down (opt_skip_intro), Right, Return (OK)
    for (int k = 0; k < 6; ++k) ofe.handleKey(SDLK_DOWN);
    ofe.handleKey(SDLK_RIGHT);
    for (int k = 0; k < 2; ++k) ofe.handleKey(SDLK_DOWN);
    ofe.handleKey(SDLK_RIGHT);
    ofe.handleMouseDown(188.0, 529.0, 1);  // OK [132,512,112,35]
    check(ofe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "OK returns from Options to MainMenu");
    check(ofe.takeSettingsSaved() && ofe.settings().smoothTerrain && ofe.settings().skipIntro,
          "OK commits Options edits and sets takeSettingsSaved()");

    // Synthetic Replay Browser flow: empty notice, then populate 2 replays and launch via double-click.
    ofe.handleKey(SDLK_P);
    check(ofe.state().screen() == ss2::FrontEnd::Screen::Replays, "P opens Replays");
    ofe.renderToBuffer(1280, 720, buf);
    ofe.handleKey(SDLK_R);
    check(ofe.state().screen() == ss2::FrontEnd::Screen::Replays && !ofe.activeNotice().empty(),
          "Empty Replay button stays on Replays with a notice");
    ofe.setReplays({
        {"tutorial1_full", "/tmp/tutorial1_full.ss2rep", "maps/single/--- TUTORIAL I ---.SSM",
         "0xeae920846989222f", 12, 3862, 2, true},
        {"russian1_smoke", "/tmp/russian1_smoke.ss2rep", "maps/campaign/000000.ssc",
         "0x1234abcd", 6, 150, 3, false},
    });
    ofe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback Replays buffer size");
    ofe.handleMouseDown(120.0, 168.0, 1);  // Row 1 [kListY+6+20 = 160..180]
    check(ofe.state().selectedReplay() == 1, "clicked replay row 1");
    ofe.handleMouseDown(120.0, 168.0, 2);  // Double click -> LaunchReplay
    check(ofe.state().screen() == ss2::FrontEnd::Screen::LaunchReplay &&
          ofe.state().launchReplayPath() == "/tmp/russian1_smoke.ss2rep",
          "Double clicking replay launches selected replay");

    // Synthetic Load Game browser flow: SinglePlayer -> Load -> empty notice -> populate 2 saves -> delete request -> double-click launch -> Esc back.
    ss2::GoldFrontEnd lfe(nullptr, {});
    lfe.handleMouseDown(188.0, 128.0, 1);  // SinglePlayer
    lfe.handleMouseDown(188.0, 329.0, 1);  // Load [134,312,108,35]
    check(lfe.state().screen() == ss2::FrontEnd::Screen::LoadGame, "clicked Load -> LoadGame");
    lfe.renderToBuffer(1280, 720, buf);
    lfe.handleKey(SDLK_L);
    check(lfe.state().screen() == ss2::FrontEnd::Screen::LoadGame && !lfe.activeNotice().empty(),
          "Empty Load button stays on LoadGame with a notice");
    lfe.setSaves({
        {"Tutorial I — Quick Save", "/tmp/tutorial1.ss2save", "maps/single/--- TUTORIAL I ---.SSM",
         "Tutorial I", "Quick Save", "2026-11-22 01:20", 1414u, 29u, 0u, 2, 12288u},
        {"1. Starting Position — Slot 1", "/tmp/023000_slot1.ss2save", "maps/campaign/023000.ssc",
         "1. Starting Position", "Slot 1", "2026-11-22 01:15", 800u, 18u, 4u, 3, 24576u},
    });
    lfe.renderToBuffer(1280, 720, buf);
    check(buf.size() == 1280u * 720u * 4u, "fallback LoadGame buffer size");
    lfe.handleMouseDown(120.0, 168.0, 1);  // Row 1
    check(lfe.state().selectedSave() == 1, "clicked save row 1");
    lfe.handleKey(SDLK_D);
    const auto delReq = lfe.takeDeleteSaveRequest();
    check(delReq && *delReq == "/tmp/023000_slot1.ss2save", "D key emits delete save request for selected save");
    check(!lfe.takeDeleteSaveRequest().has_value(), "takeDeleteSaveRequest clears pending request");
    lfe.handleMouseDown(120.0, 148.0, 2);  // Double click Row 0 -> LaunchSave
    check(lfe.state().screen() == ss2::FrontEnd::Screen::LaunchSave &&
          lfe.state().launchSavePath() == "/tmp/tutorial1.ss2save",
          "Double clicking save launches selected save");
    lfe.handleKey(SDLK_ESCAPE);
    check(lfe.state().screen() == ss2::FrontEnd::Screen::LoadGame, "Esc from LaunchSave returns to LoadGame");
    lfe.handleKey(SDLK_ESCAPE);
    check(lfe.state().screen() == ss2::FrontEnd::Screen::SinglePlayer, "Esc from LoadGame returns to SinglePlayer");
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

    const auto creditsRaw = ss2::assets::readSueEntry(menuSue, "CREDITS.PCK");
    check(ss2::assets::pckFrameCount(creditsRaw) == 1, "CREDITS.PCK 1 frame");
    const auto creditsFrame = ss2::assets::decodeUiPckFrame(creditsRaw, 0);
    check(creditsFrame.width == 640 && creditsFrame.height == 480, "CREDITS.PCK 640x480");
    const auto rgRaw = ss2::assets::readSueEntry(menuSue, "rg_bt.pck");
    check(ss2::assets::pckFrameCount(rgRaw) == 2, "rg_bt.pck 2 frames");
    for (std::size_t i = 0; i < 2; ++i) (void)ss2::assets::decodeUiPckFrame(rgRaw, i);
    const auto opRaw = ss2::assets::readSueEntry(menuSue, "op_bt.pck");
    check(ss2::assets::pckFrameCount(opRaw) == 4, "op_bt.pck 4 frames");
    for (std::size_t i = 0; i < 4; ++i) (void)ss2::assets::decodeUiPckFrame(opRaw, i);

    ss2::GoldFrontEnd fe(nullptr, gameDir, 0);
    check(fe.hasMenuAssets(), "retail menu_common.sue loaded");
    check(fe.hasCreditsArt(), "retail CREDITS.PCK loaded");
    check(fe.creditsLineCount() >= 128, "retail credits.seq loaded (128+ lines)");

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

    const auto mpList = ss2::assets::listMultiplayerMissions(gameDir);
    check(mpList.size() == 35u, "retail 35 multiplayer missions listed");
    std::vector<ss2::GoldMissionEntry> mpEntries;
    mpEntries.reserve(mpList.size());
    for (const auto& m : mpList) {
        mpEntries.push_back({m.name, m.path.string(), m.description,
                             m.scheme == 2 ? "Beach" : "Summer",
                             m.width, m.height, m.scheme, false, 0});
    }
    fe.setMultiplayerMissions(std::move(mpEntries));

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

    // Screen 2c: Custom Campaigns (retail empty state)
    fe.handleMouseDown(188.0, 529.0, 1);  // Custom [118,512,140,35]
    check(fe.state().screen() == ss2::FrontEnd::Screen::CustomCampaigns, "retail CustomCampaigns");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen2c_custom_campaigns.png", 1280, 720, frame);
    fe.handleKey(SDLK_ESCAPE);
    check(fe.state().screen() == ss2::FrontEnd::Screen::Campaigns, "Esc returns from CustomCampaigns");

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

    // Screen 4b: Campaign Briefing (with retail animated overlay & live controls)
    fe.handleMouseDown(188.0, 529.0, 1);  // Start -> Briefing
    check(fe.state().screen() == ss2::FrontEnd::Screen::Briefing &&
          fe.state().briefingFromCampaign(), "retail Campaign Briefing");
    check(fe.hasBriefingMedia() && fe.briefingPartCount() == 4u &&
          fe.briefingActivePart() == 0 && !fe.briefingPaused(),
          "023000.ssc loads 4 animated briefing parts on entering Briefing");
    const std::string overviewText = fe.briefingDisplayedText();
    check(!overviewText.empty() && overviewText.find('~') == std::string::npos,
          "briefingDisplayedText shows overview paragraph without '~'");

    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen4b_campaign_briefing.png", 1280, 720, frame);
    const auto frame0 = frame;
    fe.advance(std::chrono::milliseconds(500));
    check(fe.briefingCurrentFrame() >= 14u, "animated briefing advances frame over time");
    fe.renderToBuffer(1280, 720, frame);
    check(frame != frame0, "animated briefing updates rendered pixels");

    fe.handleKey(SDLK_P);  // Pause
    check(fe.briefingPaused(), "P toggles briefing pause on");
    const std::size_t pausedFrame = fe.briefingCurrentFrame();
    fe.advance(std::chrono::milliseconds(200));
    check(fe.briefingCurrentFrame() == pausedFrame, "paused briefing freezes animation frame");
    fe.handleKey(SDLK_P);  // Resume
    check(!fe.briefingPaused(), "P toggles briefing pause off");

    fe.handleKey(SDLK_1);  // Select Part 1
    check(fe.briefingActivePart() == 1 && fe.briefingCurrentFrame() == 0u,
          "key 1 selects Part 1 from frame 0");
    const std::string part1Text = fe.briefingDisplayedText();
    check(!part1Text.empty() && part1Text != overviewText && part1Text.find('~') == std::string::npos,
          "selecting Part 1 switches displayed paragraph");
    fe.advance(std::chrono::milliseconds(150));
    check(fe.briefingCurrentFrame() > 0u, "selected Part 1 advances frames");

    fe.handleKey(SDLK_R);  // Restart overview
    check(fe.briefingActivePart() == 0 && fe.briefingCurrentFrame() == 0u && !fe.briefingPaused() &&
          fe.briefingDisplayedText() == overviewText,
          "R restarts briefing back to overview");

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
    check(!fe.hasBriefingMedia() && fe.briefingPartCount() == 0u && !fe.briefingPaused(),
          "Tutorial I has no animated briefing media");
    fe.handleKey(SDLK_R);
    fe.handleKey(SDLK_P);
    check(!fe.briefingPaused() && fe.briefingActivePart() == 0,
          "R/P are safe no-ops on non-animated briefing");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen4_briefing.png", 1280, 720, frame);
    fe.renderToBuffer(3840, 2160, frame);
    check(frame.size() == 3840u * 2160u * 4u, "retail Screen 4 4K buffer");
    if (!capDir.empty()) writePng(capDir / "screen4_4k.png", 3840, 2160, frame);

    // Return to MainMenu, capture Ratings (Campaigns & Single Missions) and Credits
    fe.handleKey(SDLK_ESCAPE);  // Briefing -> SingleMissions
    fe.handleKey(SDLK_ESCAPE);  // SingleMissions -> SinglePlayer
    fe.handleKey(SDLK_ESCAPE);  // SinglePlayer -> MainMenu
    check(fe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "returned to MainMenu for Ratings/Credits");

    fe.handleKey(SDLK_R);
    check(fe.state().screen() == ss2::FrontEnd::Screen::Ratings, "retail Ratings");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen5_ratings_campaigns.png", 1280, 720, frame);
    fe.handleKey(SDLK_S);
    check(fe.state().ratingsMode() == ss2::FrontEnd::RatingsMode::SingleMissions, "retail Ratings SingleMissions");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen5_ratings_missions.png", 1280, 720, frame);
    fe.handleKey(SDLK_ESCAPE);

    fe.handleKey(SDLK_C);
    check(fe.state().screen() == ss2::FrontEnd::Screen::Credits, "retail Credits");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen6_credits.png", 1280, 720, frame);
    for (int step = 0; step < 12; ++step) fe.advance(std::chrono::milliseconds(200));
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen6_credits_scrolled.png", 1280, 720, frame);
    fe.handleKey(SDLK_ESCAPE);
    check(fe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "returned from Credits to MainMenu");

    // Capture Options and Replay Browser screens
    fe.setSettings(ss2::UserSettings{});
    fe.handleKey(SDLK_O);
    check(fe.state().screen() == ss2::FrontEnd::Screen::Options, "retail Options");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen7_options.png", 1280, 720, frame);
    fe.handleKey(SDLK_ESCAPE);

    fe.handleKey(SDLK_P);
    check(fe.state().screen() == ss2::FrontEnd::Screen::Replays, "retail Replays empty");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen8_replays_empty.png", 1280, 720, frame);
    fe.setReplays({
        {"tutorial1_p7_full", "/tmp/tutorial1_p7_full.ss2rep", "maps/single/--- TUTORIAL I ---.SSM",
         "0xeae920846989222f", 12, 3862, 2, true},
        {"russian1_campaign", "/tmp/russian1_campaign.ss2rep", "maps/campaign/000000.ssc",
         "0x7f13a8c901234567", 4, 150, 3, true},
    });
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen8_replays.png", 1280, 720, frame);
    fe.handleKey(SDLK_ESCAPE);
    check(fe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "returned from Replays to MainMenu");

    // Capture Load Game browser screen (empty & populated)
    fe.handleKey(SDLK_S);
    check(fe.state().screen() == ss2::FrontEnd::Screen::SinglePlayer, "S opens SinglePlayer");
    fe.handleKey(SDLK_L);
    check(fe.state().screen() == ss2::FrontEnd::Screen::LoadGame, "retail LoadGame empty");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen9_load_empty.png", 1280, 720, frame);
    fe.setSaves({
        {"Tutorial I — Quick Save", "/tmp/tutorial1.ss2save", "maps/single/--- TUTORIAL I ---.SSM",
         "Tutorial I", "Quick Save", "2026-11-22 01:20", 1414u, 29u, 0u, 2, 12288u},
        {"1. Starting Position — Auto-Save", "/tmp/023000_autosave.ss2save", "maps/campaign/023000.ssc",
         "1. Starting Position", "Auto-Save", "2026-11-22 01:10", 7500u, 24u, 6u, 2, 24576u},
    });
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen9_load.png", 1280, 720, frame);
    fe.handleKey(SDLK_ESCAPE);
    fe.handleKey(SDLK_ESCAPE);
    check(fe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "returned from LoadGame to MainMenu");

    fe.handleKey(SDLK_M);
    check(fe.state().screen() == ss2::FrontEnd::Screen::Multiplayer, "retail Multiplayer");
    fe.renderToBuffer(1280, 720, frame);
    if (!capDir.empty()) writePng(capDir / "screen10_multiplayer.png", 1280, 720, frame);
    fe.handleKey(SDLK_ESCAPE);
    check(fe.state().screen() == ss2::FrontEnd::Screen::MainMenu, "returned from Multiplayer to MainMenu");
}

struct ProgressBarStats {
    std::uint64_t totalRgb{};
    std::uint64_t barRgb{};
    std::uint64_t lampRgb{};
    std::size_t changedBarPixels{};
    std::size_t changedRightHalfBarPixels{};
};

ProgressBarStats measureLoadingFrame800x600(const std::vector<std::uint8_t>& rgba,
                                            const std::vector<std::uint8_t>& baseline0) {
    check(rgba.size() == 800u * 600u * 4u, "loading frame 800x600 RGBA size");
    check(baseline0.size() == 800u * 600u * 4u, "loading baseline 800x600 RGBA size");
    ProgressBarStats s{};
    for (std::size_t i = 0; i < rgba.size(); i += 4u) {
        s.totalRgb += std::uint64_t(rgba[i]) + rgba[i + 1] + rgba[i + 2];
    }
    // Progress channel in 800x600 stage space: x=[138, 654), y=[312, 316)
    for (int y = 312; y < 316; ++y) {
        for (int x = 138; x < 654; ++x) {
            const std::size_t p = (std::size_t(y) * 800u + std::size_t(x)) * 4u;
            s.barRgb += std::uint64_t(rgba[p]) + rgba[p + 1] + rgba[p + 2];
            if (rgba[p] != baseline0[p] || rgba[p + 1] != baseline0[p + 1] || rgba[p + 2] != baseline0[p + 2]) {
                ++s.changedBarPixels;
                if (x >= 400) ++s.changedRightHalfBarPixels;
            }
        }
    }
    // Completion indicator lamp in 800x600 stage space: x=[661, 675), y=[310, 318)
    for (int y = 310; y < 318; ++y) {
        for (int x = 661; x < 675; ++x) {
            const std::size_t p = (std::size_t(y) * 800u + std::size_t(x)) * 4u;
            s.lampRgb += std::uint64_t(rgba[p]) + rgba[p + 1] + rgba[p + 2];
        }
    }
    return s;
}

void testLoadingScreenFallback() {
    ss2::GoldLoadingScreen loader(nullptr, {});
    check(!loader.hasRetailAssets(), "asset-free GoldLoadingScreen hasRetailAssets=false");
    check(loader.recordCount() == 0u, "asset-free GoldLoadingScreen recordCount=0");

    std::vector<std::uint8_t> f0, f50, f100;
    loader.renderToBuffer(800, 600, "Tutorial I", 0.0f, f0);
    loader.renderToBuffer(800, 600, "Tutorial I", 0.5f, f50);
    loader.renderToBuffer(800, 600, "Tutorial I", 1.0f, f100);

    const auto s0 = measureLoadingFrame800x600(f0, f0);
    const auto s50 = measureLoadingFrame800x600(f50, f0);
    const auto s100 = measureLoadingFrame800x600(f100, f0);

    check(s0.totalRgb > 0 && s50.totalRgb > 0 && s100.totalRgb > 0, "fallback loading frames non-empty");
    check(s50.changedBarPixels > 800u && s100.changedBarPixels > s50.changedBarPixels,
          "fallback progress bar pixels increase monotonically 0.0 -> 0.5 -> 1.0");
    check(s50.changedRightHalfBarPixels == 0u && s100.changedRightHalfBarPixels > 800u,
          "fallback right half of progress bar fills between 0.5 and 1.0");
    check(s0.barRgb < s50.barRgb && s50.barRgb < s100.barRgb,
          "fallback progress bar luminance increases monotonically");
    check(s0.lampRgb == s50.lampRgb && s100.lampRgb > s50.lampRgb,
          "fallback completion lamp lights at 1.0");
}

void testRetailLoadingScreen(const std::filesystem::path& gameDir) {
    const auto lngSue = ss2::assets::resolveCaseInsensitivePath(gameDir, "lng.sue");
    check(std::filesystem::exists(lngSue), "lng.sue exists for Gold Screen 5 loadscr.dat");
    const auto rawLoadScr = ss2::assets::readSueEntry(lngSue, "loadscr.dat");
    check(rawLoadScr.size() > 1000000u, "retail loadscr.dat unpacked (>1 MB)");

    const char* capEnv = std::getenv("SS2_GOLD_FE_CAPTURE_DIR");
    const std::filesystem::path capDir = capEnv ? std::filesystem::path(capEnv) : std::filesystem::path{};

    for (int bg = 0; bg < 3; ++bg) {
        ss2::GoldLoadingScreen loader(nullptr, gameDir, bg);
        check(loader.hasRetailAssets(), "retail GoldLoadingScreen loaded loadscr.dat");
        check(loader.backgroundIndex() == bg, "retail GoldLoadingScreen honored fixedBgIndex");
        check(loader.recordCount() == 17u, "retail loadscr.dat has 17 records");

        std::vector<std::uint8_t> f0, f50, f100;
        loader.renderToBuffer(800, 600, "1. Starting Position", 0.0f, f0);
        loader.renderToBuffer(800, 600, "1. Starting Position", 0.5f, f50);
        loader.renderToBuffer(800, 600, "1. Starting Position", 1.0f, f100);

        const auto s0 = measureLoadingFrame800x600(f0, f0);
        const auto s50 = measureLoadingFrame800x600(f50, f0);
        const auto s100 = measureLoadingFrame800x600(f100, f0);

        check(s0.totalRgb > 0 && s50.totalRgb > 0 && s100.totalRgb > 0, "retail loading frames non-empty");
        check(s50.changedBarPixels > 800u && s100.changedBarPixels > s50.changedBarPixels,
              "retail progress bar pixels increase monotonically 0.0 -> 0.5 -> 1.0");
        check(s50.changedRightHalfBarPixels == 0u && s100.changedRightHalfBarPixels > 800u,
              "retail right half of progress bar fills between 0.5 and 1.0");
        check(s0.barRgb < s50.barRgb && s50.barRgb < s100.barRgb,
              "retail progress bar luminance increases monotonically");
        check(s0.lampRgb == s50.lampRgb && s100.lampRgb > s50.lampRgb,
              "retail completion lamp lights at 1.0");

        if (!capDir.empty() && bg == 0) {
            std::vector<std::uint8_t> hdFrame;
            loader.renderToBuffer(1280, 720, "1. Starting Position", 0.5f, hdFrame);
            writePng(capDir / "screen5_loading_50.png", 1280, 720, hdFrame);
            loader.renderToBuffer(1280, 720, "1. Starting Position", 1.0f, hdFrame);
            writePng(capDir / "screen5_loading_100.png", 1280, 720, hdFrame);
        }
    }
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
    {
        ss2::GoldFrontEnd headlessFe(nullptr, {});
        check(!headlessFe.hasMenuCursor(), "headless GoldFrontEnd hasMenuCursor=false");
        ss2::GoldPostMissionScreen headlessPm(nullptr, {});
        check(!headlessPm.hasMenuCursor(), "headless GoldPostMissionScreen hasMenuCursor=false");
    }
    ss2::GoldFrontEnd fe(win, gameDir, 0);
    check(fe.hasMenuCursor(), "live retail GoldFrontEnd loads 21-frame MENU_CUR.ANI/.COL cursor");
    fe.setMissions(makeSyntheticMissions(3));
    fe.setCampaigns(makeSyntheticCampaigns());
    fe.setMultiplayerMissions(makeSyntheticMissions(3));
    const ss2::Snapshot empty{};
    for (int i = 0; i < 23; ++i) {
        if (i == 1) fe.handleKey(SDLK_O);       // Options
        if (i == 2) fe.handleKey(SDLK_ESCAPE);  // Back to MainMenu
        if (i == 3) fe.handleKey(SDLK_P);       // Replays
        if (i == 4) fe.handleKey(SDLK_ESCAPE);  // Back to MainMenu
        if (i == 5) fe.handleKey(SDLK_R);       // Ratings (Campaigns)
        if (i == 6) fe.handleKey(SDLK_S);       // Ratings (Single Missions)
        if (i == 7) fe.handleKey(SDLK_ESCAPE);  // Back to MainMenu
        if (i == 8) fe.handleKey(SDLK_C);       // Credits
        if (i == 9) fe.advance(std::chrono::milliseconds(200));
        if (i == 10) fe.handleKey(SDLK_ESCAPE); // Back to MainMenu
        if (i == 11) fe.handleKey(SDLK_S);      // Single Player
        if (i == 12) fe.handleKey(SDLK_L);      // Load Game
        if (i == 13) fe.handleKey(SDLK_ESCAPE); // Back to Single Player
        if (i == 14) fe.handleKey(SDLK_C);      // Campaigns
        if (i == 15) fe.handleKey(SDLK_C);      // Custom Campaigns
        if (i == 16) fe.handleKey(SDLK_ESCAPE); // Back to Campaigns
        if (i == 17) fe.handleKey(SDLK_G);      // German Campaign missions
        if (i == 18) fe.handleKey(SDLK_D);      // Cycle difficulty
        if (i == 19) fe.handleKey(SDLK_RETURN); // Campaign Briefing
        if (i == 20) { fe.handleKey(SDLK_ESCAPE); fe.handleKey(SDLK_ESCAPE); fe.handleKey(SDLK_ESCAPE); fe.handleKey(SDLK_ESCAPE); } // Back to MainMenu
        if (i == 21) fe.handleKey(SDLK_M);      // Multiplayer
        if (i == 22) fe.handleKey(SDLK_ESCAPE); // Back to MainMenu
        fe.draw(renderer);
        renderer.present(empty);
    }

    ss2::GoldLoadingScreen loading(win, gameDir, 0);
    loading.drawAndPresent(renderer, "1. Starting Position", 0.0f);
    loading.drawAndPresent(renderer, "1. Starting Position", 0.5f);
    loading.drawAndPresent(renderer, "1. Starting Position", 1.0f);
    loading.clear(renderer);
    renderer.present(empty);

    ss2::GoldPostMissionScreen postStats(win, gameDir, 0);
    ss2::GoldPostMissionStats winStats{};
    winStats.victory = true;
    winStats.missionTitle = "1. Operation Overlord";
    winStats.commanderName = "BritCmdr";
    winStats.faction = ss2::GoldMedalFaction::Allied;
    winStats.elapsedSeconds = 742u;
    winStats.enemyInfantryKilled = 48u;
    winStats.enemyTanksDestroyed = 7u;
    winStats.enemyVehiclesDestroyed = 11u;
    winStats.enemyGunsDestroyed = 5u;
    winStats.enemyPlanesShotDown = 2u;
    winStats.buildingsDemolished = 3u;
    winStats.airSortiesFlown = 2u;
    winStats.friendlyCasualties = 9u;
    winStats.battleScore = 6840;
    winStats.bestScore = 6840;
    winStats.isNewBest = true;
    winStats.starRating = 8;
    winStats.hasNextCampaign = true;
    winStats.nextCampaignTitle = "2. Breakout";
    postStats.open(winStats, false);
    check(postStats.hasMenuCursor(), "live retail GoldPostMissionScreen loads MENU_CUR.ANI/.COL cursor");
    postStats.draw(renderer);
    renderer.present(empty);

    postStats.handleKey(SDLK_DOWN);
    postStats.draw(renderer);
    renderer.present(empty);

    winStats.victory = false;
    winStats.isNewBest = false;
    winStats.starRating = 0;
    winStats.hasNextCampaign = false;
    postStats.open(winStats, false);
    check(postStats.hasMenuCursor(), "live retail GoldPostMissionScreen loads MENU_CUR.ANI/.COL cursor");
    postStats.draw(renderer);
    renderer.present(empty);

    postStats.clear(renderer);
    renderer.present(empty);

    check(report.errors.load() == 0,
          "Zero Vulkan validation errors on GoldFrontEnd, GoldLoadingScreen, and GoldPostMissionScreen overlays");
}

std::uint64_t sumRegionRgb(const std::vector<std::uint8_t>& rgba, int width,
                           int x0, int y0, int x1, int y1) {
    std::uint64_t sum = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t p = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4u;
            sum += std::uint64_t(rgba[p]) + rgba[p + 1] + rgba[p + 2];
        }
    }
    return sum;
}

std::size_t countDiffPixels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                            int width, int x0, int y0, int x1, int y1) {
    std::size_t diff = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t p = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4u;
            if (a[p] != b[p] || a[p + 1] != b[p + 1] || a[p + 2] != b[p + 2]) ++diff;
        }
    }
    return diff;
}

void testPostMissionScreenFallback() {
    ss2::GoldPostMissionScreen pm(nullptr, {});
    check(!pm.hasRetailAssets(), "asset-free GoldPostMissionScreen hasRetailAssets=false");
    check(!pm.hasFanfareAudio(), "asset-free GoldPostMissionScreen hasFanfareAudio=false");
    check(!pm.active(), "GoldPostMissionScreen starts inactive");

    ss2::GoldPostMissionStats st{};
    st.victory = true;
    st.missionTitle = "Tutorial I";
    st.commanderName = "CmdrTest";
    st.faction = ss2::GoldMedalFaction::Allied;
    st.elapsedSeconds = 365u;
    st.enemyInfantryKilled = 24u;
    st.enemyTanksDestroyed = 3u;
    st.enemyVehiclesDestroyed = 6u;
    st.enemyGunsDestroyed = 2u;
    st.enemyPlanesShotDown = 1u;
    st.buildingsDemolished = 2u;
    st.airSortiesFlown = 1u;
    st.friendlyCasualties = 4u;
    st.battleScore = 4200;
    st.bestScore = 4200;
    st.isNewBest = true;
    st.starRating = 0;
    st.hasNextCampaign = true;
    st.nextCampaignTitle = "2. River Crossing";

    pm.open(st, false);
    check(pm.active(), "GoldPostMissionScreen active after open()");
    check(pm.focusedId() == "next_campaign", "Victory with nextCampaign focuses next_campaign first");
    const auto ids = pm.actionIds();
    check(ids.size() == 6u && ids[0] == "next_campaign" && ids[1] == "replay" &&
              ids[2] == "campaigns" && ids[3] == "missions" && ids[4] == "resume" && ids[5] == "quit",
          "Victory actionIds match full debrief order");

    std::vector<std::uint8_t> stars0, stars5, stars10;
    pm.renderToBuffer(800, 600, stars0);
    st.starRating = 5;
    pm.open(st, false);
    pm.renderToBuffer(800, 600, stars5);
    st.starRating = 10;
    pm.open(st, false);
    pm.renderToBuffer(800, 600, stars10);

    check(stars0.size() == 800u * 600u * 4u, "fallback 800x600 RGBA buffer size");
    const auto lum0 = sumRegionRgb(stars0, 800, 484, 326, 738, 378);
    const auto lum5 = sumRegionRgb(stars5, 800, 484, 326, 738, 378);
    const auto lum10 = sumRegionRgb(stars10, 800, 484, 326, 738, 378);
    check(lum0 > 0u && lum5 > lum0 && lum10 > lum5,
          "fallback star medal rack luminance increases monotonically 0 -> 5 -> 10 stars");

    // Keyboard navigation & hotkeys
    bool closed = false;
    check(pm.handleKey(SDLK_DOWN, &closed).empty() && !closed && pm.focusedId() == "replay",
          "SDLK_DOWN advances focus to replay");
    check(pm.handleKey(SDLK_RIGHT, &closed).empty() && pm.focusedId() == "campaigns",
          "SDLK_RIGHT advances focus to campaigns");
    check(pm.handleKey(SDLK_LEFT, &closed).empty() && pm.focusedId() == "replay",
          "SDLK_LEFT moves focus back to replay");
    check(pm.handleKey(SDLK_END, &closed).empty() && pm.focusedId() == "quit",
          "SDLK_END jumps focus to quit");
    check(pm.handleKey(SDLK_HOME, &closed).empty() && pm.focusedId() == "next_campaign",
          "SDLK_HOME jumps focus to next_campaign");
    check(pm.handleKey(SDLK_RETURN, &closed) == "next_campaign" && !closed,
          "SDLK_RETURN activates focused button");
    check(pm.handleKey(SDLK_R, &closed) == "replay", "hotkey R activates replay");
    check(pm.handleKey(SDLK_Q, &closed) == "quit", "hotkey Q activates quit");

    // Mouse hover + click hit-testing across every button
    for (const auto& id : ids) {
        const auto ctr = pm.actionStageCenter(id);
        check(ctr.has_value(), "actionStageCenter returns valid coords for every button");
        pm.handleMouseMove(ctr->first, ctr->second);
        check(pm.hoveredId() == id && pm.focusedId() == id, "mouse hover updates hoveredId & focusedId");
        check(pm.handleMouseDown(ctr->first, ctr->second) == id, "mouse click activates hovered button");
        pm.handleMouseUp();
    }

    // Escape closes
    check(pm.handleKey(SDLK_ESCAPE, &closed).empty() && closed && !pm.active(),
          "SDLK_ESCAPE closes GoldPostMissionScreen");

    // Defeat omits next_campaign and renders distinct pixels from Victory
    st.victory = false;
    st.isNewBest = false;
    st.starRating = 0;
    pm.open(st, false);
    check(pm.focusedId() == "replay", "Defeat focuses replay first");
    check(pm.actionIds().size() == 5u, "Defeat omits next_campaign");
    std::vector<std::uint8_t> defeatBuf;
    pm.renderToBuffer(800, 600, defeatBuf);
    check(countDiffPixels(stars10, defeatBuf, 800, 0, 0, 800, 600) > 20000u,
          "Fallback Victory vs Defeat renders substantially distinct frames");
}

void testRetailPostMissionScreen(const std::filesystem::path& gameDir) {
    const auto menuSue = ss2::assets::resolveCaseInsensitivePath(gameDir, "menu_common.sue");
    check(std::filesystem::exists(menuSue), "menu_common.sue exists for GoldPostMissionScreen");
    check(ss2::assets::pckFrameCount(ss2::assets::readSueEntry(menuSue, "stat_pic.pck")) >= 9u,
          "retail stat_pic.pck has 9 unit silhouette frames");
    check(ss2::assets::pckFrameCount(ss2::assets::readSueEntry(menuSue, "mstat_bt.pck")) >= 6u,
          "retail mstat_bt.pck has 6 button frames");
    check(ss2::assets::pckFrameCount(ss2::assets::readSueEntry(menuSue, "ZVEZDY.PCK")) >= 3u,
          "retail ZVEZDY.PCK has 3 faction star frames");
    check(ss2::assets::pckFrameCount(ss2::assets::readSueEntry(menuSue, "ZV_GOLD.PCK")) >= 15u,
          "retail ZV_GOLD.PCK has 15 gold star halo frames");
    check(ss2::assets::pckFrameCount(ss2::assets::readSueEntry(menuSue, "ZV_M.PCK")) >= 3u,
          "retail ZV_M.PCK has 3 compact star frames");
    check(!ss2::assets::readSueEntry(menuSue, "win_rus.pck").empty(), "retail win_rus.pck present");
    check(!ss2::assets::readSueEntry(menuSue, "win_jap.pck").empty(), "retail win_jap.pck present");
    check(ss2::assets::readSueEntry(menuSue, "winwav").size() > 7000000u, "retail winwav PCM present (>7 MB)");
    check(ss2::assets::readSueEntry(menuSue, "loosewav").size() > 16000000u, "retail loosewav PCM present (>16 MB)");

    ss2::GoldPostMissionScreen pm(nullptr, gameDir, 0);
    check(pm.hasRetailAssets(), "retail GoldPostMissionScreen loaded retail sprites");
    check(pm.hasFanfareAudio(), "retail GoldPostMissionScreen decoded winwav & loosewav fanfare audio");

    const char* capEnv = std::getenv("SS2_GOLD_FE_CAPTURE_DIR");
    const std::filesystem::path capDir = capEnv ? std::filesystem::path(capEnv) : std::filesystem::path{};

    ss2::GoldPostMissionStats alliedWin{};
    alliedWin.victory = true;
    alliedWin.missionTitle = "1. Operation Overlord";
    alliedWin.commanderName = "Eisenhower";
    alliedWin.faction = ss2::GoldMedalFaction::Allied;
    alliedWin.elapsedSeconds = 874u;
    alliedWin.enemyInfantryKilled = 63u;
    alliedWin.enemyTanksDestroyed = 9u;
    alliedWin.enemyVehiclesDestroyed = 14u;
    alliedWin.enemyGunsDestroyed = 6u;
    alliedWin.enemyPlanesShotDown = 3u;
    alliedWin.buildingsDemolished = 4u;
    alliedWin.airSortiesFlown = 3u;
    alliedWin.friendlyCasualties = 12u;
    alliedWin.battleScore = 7650;
    alliedWin.bestScore = 7650;
    alliedWin.isNewBest = true;
    alliedWin.starRating = 8;
    alliedWin.hasNextCampaign = true;
    alliedWin.nextCampaignTitle = "2. Hedgerow Breakout";

    std::vector<std::uint8_t> allied0, allied8, rusWin, japWin, axisDefeat;
    alliedWin.starRating = 0;
    pm.open(alliedWin, false);
    pm.renderToBuffer(800, 600, allied0);

    alliedWin.starRating = 8;
    pm.open(alliedWin, false);
    pm.renderToBuffer(800, 600, allied8);

    check(countDiffPixels(allied0, allied8, 800, 484, 326, 738, 378) > 350u,
          "retail ZV_GOLD.PCK / ZVEZDY.PCK star sprites change medal rack pixels for 8 stars vs 0");

    auto rusStats = alliedWin;
    rusStats.missionTitle = "5. Berlin Assault";
    rusStats.commanderName = "Zhukov";
    rusStats.faction = ss2::GoldMedalFaction::Soviet;
    rusStats.isRussianCampaign = true;
    rusStats.starRating = 10;
    rusStats.hasNextCampaign = false;
    pm.open(rusStats, false);
    pm.renderToBuffer(800, 600, rusWin);

    auto japStats = alliedWin;
    japStats.missionTitle = "1. Omen of Victory";
    japStats.commanderName = "Yamamoto";
    japStats.faction = ss2::GoldMedalFaction::Axis;
    japStats.isJapaneseCampaign = true;
    japStats.starRating = 7;
    pm.open(japStats, false);
    pm.renderToBuffer(800, 600, japWin);

    auto defStats = alliedWin;
    defStats.victory = false;
    defStats.faction = ss2::GoldMedalFaction::Axis;
    defStats.isNewBest = false;
    defStats.starRating = 0;
    defStats.hasNextCampaign = false;
    pm.open(defStats, false);
    pm.renderToBuffer(800, 600, axisDefeat);

    check(countDiffPixels(allied8, rusWin, 800, 0, 0, 800, 600) > 100000u,
          "retail win_rus.pck backdrop renders distinct Russian campaign victory screen");
    check(countDiffPixels(rusWin, japWin, 800, 0, 0, 800, 600) > 100000u,
          "retail win_jap.pck backdrop renders distinct Japanese campaign victory screen");
    check(countDiffPixels(allied8, axisDefeat, 800, 0, 0, 800, 600) > 30000u,
          "retail Defeat screen renders distinct visuals from Victory screen");

    if (!capDir.empty()) {
        std::filesystem::create_directories(capDir);
        std::vector<std::uint8_t> hdFrame;
        pm.open(alliedWin, false);
        pm.renderToBuffer(1280, 720, hdFrame);
        writePng(capDir / "postmission_victory_allied.png", 1280, 720, hdFrame);
        pm.open(rusStats, false);
        pm.renderToBuffer(800, 600, hdFrame);
        writePng(capDir / "postmission_victory_russian.png", 800, 600, hdFrame);
        pm.open(defStats, false);
        pm.renderToBuffer(1280, 720, hdFrame);
        writePng(capDir / "postmission_defeat.png", 1280, 720, hdFrame);
    }
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
    testLoadingScreenFallback();
    testPostMissionScreenFallback();

    std::filesystem::path gameDir;
    if (const char* env = std::getenv("SS2_PRIVATE_GAME"); env && *env) {
        gameDir = env;
    }
    if (!gameDir.empty() && std::filesystem::is_directory(gameDir)) {
        testRetailGoldAssets(gameDir);
        testRetailLoadingScreen(gameDir);
        testRetailPostMissionScreen(gameDir);
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
