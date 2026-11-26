#pragma once
#include "assets/indexed.hpp"
#include "assets/sprite.hpp"
#include "ui/hidpi.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ss2 {

namespace cursor_detail {
using CursorPtr = std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)>;

/// Determines integer cursor scale (1 or 2) from window pixel height / display DPI scale.
inline int cursorScaleForWindow(SDL_Window* window) noexcept {
    if (!window) return 1;
    float uiScale = 1.0f;
    int pixelW = 0, pixelH = 0;
    if (SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH) && pixelH > 0) {
        uiScale = std::max(uiScale, static_cast<float>(pixelH) / 1080.0f);
    }
    const float dispScale = SDL_GetWindowDisplayScale(window);
    if (dispScale > 0.0f) {
        uiScale = std::max(uiScale, dispScale);
    }
    return uiScale >= 1.75f ? 2 : 1;
}

/// Upscales an RGBA32 frame cleanly by 2x using the EPX pixel-art scaling algorithm.
inline void upscale2xEpxRgba(const std::uint8_t* src, unsigned sw, unsigned sh,
                             std::vector<std::uint8_t>& outRgba) {
    outRgba.clear();
    if (!src || sw == 0u || sh == 0u) return;
    const unsigned dw = sw * 2u;
    const unsigned dh = sh * 2u;
    outRgba.assign(std::size_t(dw) * std::size_t(dh) * 4u, 0u);
    const auto px = [&](int x, int y) -> const std::uint8_t* {
        const unsigned cx = static_cast<unsigned>(std::clamp(x, 0, static_cast<int>(sw) - 1));
        const unsigned cy = static_cast<unsigned>(std::clamp(y, 0, static_cast<int>(sh) - 1));
        return src + (std::size_t(cy) * sw + cx) * 4u;
    };
    const auto eq = [](const std::uint8_t* a, const std::uint8_t* b) -> bool {
        if (a[3] == 0u && b[3] == 0u) return true;
        if (a[3] == 0u || b[3] == 0u) return false;
        return std::abs(int(a[0]) - int(b[0])) <= 14 &&
               std::abs(int(a[1]) - int(b[1])) <= 14 &&
               std::abs(int(a[2]) - int(b[2])) <= 14 &&
               std::abs(int(a[3]) - int(b[3])) <= 32;
    };
    const auto put = [&](unsigned dx, unsigned dy, const std::uint8_t* c) {
        auto* out = outRgba.data() + (std::size_t(dy) * dw + dx) * 4u;
        out[0] = c[0];
        out[1] = c[1];
        out[2] = c[2];
        out[3] = c[3];
    };
    for (int y = 0; y < static_cast<int>(sh); ++y) {
        for (int x = 0; x < static_cast<int>(sw); ++x) {
            const auto* p = px(x, y);
            const auto* a = (y > 0) ? px(x, y - 1) : p;
            const auto* d = (y + 1 < static_cast<int>(sh)) ? px(x, y + 1) : p;
            const auto* c = (x > 0) ? px(x - 1, y) : p;
            const auto* b = (x + 1 < static_cast<int>(sw)) ? px(x + 1, y) : p;
            const std::uint8_t* q0 = p;
            const std::uint8_t* q1 = p;
            const std::uint8_t* q2 = p;
            const std::uint8_t* q3 = p;
            if (!eq(c, b) && !eq(a, d)) {
                if (eq(c, a)) q0 = c;
                if (eq(a, b)) q1 = b;
                if (eq(d, c)) q2 = c;
                if (eq(b, d)) q3 = b;
            }
            const unsigned ux = static_cast<unsigned>(x) * 2u;
            const unsigned uy = static_cast<unsigned>(y) * 2u;
            put(ux, uy, q0);
            put(ux + 1u, uy, q1);
            put(ux, uy + 1u, q2);
            put(ux + 1u, uy + 1u, q3);
        }
    }
}

inline std::vector<CursorPtr> buildCursorsFromChain(std::span<const std::filesystem::path> archives,
                                                    std::string_view aniEntry,
                                                    std::string_view colEntry,
                                                    int scaleFactor = 1) {
    std::vector<CursorPtr> out;
    if (archives.empty() || !SDL_WasInit(SDL_INIT_VIDEO)) return out;
    const int scale = (scaleFactor >= 2) ? 2 : 1;
    try {
        const auto ani = assets::tryReadSueEntryChain(archives, aniEntry);
        const auto col = assets::tryReadSueEntryChain(archives, colEntry);
        if (!ani || !col) return out;
        auto seq = assets::decodeAniSequence(*ani, *col);
        if (seq.frames.empty() || seq.width == 0u || seq.height == 0u) return out;
        const int baseHotX = std::clamp(seq.hotX, 0, int(seq.width) - 1);
        const int baseHotY = std::clamp(seq.hotY, 0, int(seq.height) - 1);
        const int hotX = (scale == 2) ? baseHotX * 2 : baseHotX;
        const int hotY = (scale == 2) ? baseHotY * 2 : baseHotY;
        out.reserve(seq.frames.size());
        std::vector<std::uint8_t> scaledBuf;
        for (auto& frame : seq.frames) {
            if (frame.width == 0u || frame.height == 0u) continue;
            int w = int(frame.width);
            int h = int(frame.height);
            std::uint8_t* pixels = frame.rgba.data();
            if (scale >= 2) {
                upscale2xEpxRgba(frame.rgba.data(), frame.width, frame.height, scaledBuf);
                if (scaledBuf.empty()) continue;
                w *= 2;
                h *= 2;
                pixels = scaledBuf.data();
            }
            const int frameHotX = std::clamp(hotX, 0, w - 1);
            const int frameHotY = std::clamp(hotY, 0, h - 1);
            SDL_Surface* surf = SDL_CreateSurfaceFrom(
                w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
            if (!surf) continue;
            SDL_Cursor* cur = SDL_CreateColorCursor(surf, frameHotX, frameHotY);
            SDL_DestroySurface(surf);
            if (cur) out.emplace_back(cur, SDL_DestroyCursor);
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

inline std::vector<CursorPtr> buildCursorsFromSue(const std::filesystem::path& archivePath,
                                                  std::string_view aniEntry,
                                                  std::string_view colEntry,
                                                  int scaleFactor = 1) {
    if (archivePath.empty()) return {};
    const std::array<std::filesystem::path, 1> single{archivePath};
    return buildCursorsFromChain(single, aniEntry, colEntry, scaleFactor);
}
} // namespace cursor_detail

enum class TacticalCursorKind : std::size_t {
    Field = 0,       // C1FIELD: default battlefield / HUD arrow
    OwnUnit,         // C2OWN: friendly player-owned unit hover
    FriendUnit,      // C3FRIEND: allied unit hover
    EnemyUnit,       // C4ENEMY: spotted hostile unit hover
    Forbidden,       // C6NO: impassable / invalid order
    Unload,          // C7UNLOAD: unload / evacuate
    Move,            // C8MOVE: move / reverse / escort
    AttackUnit,      // C9ATTACK: direct attack on unit/building
    AttackGround,    // C10AGRND: direct attack-ground
    AttackMove,      // C11AMOVE: assault / patrol
    KatyushaFire,    // C12KATFR: rocket artillery barrage
    HowitzerFire,    // C13HOWFR: howitzer / mortar barrage
    Reload,          // C14RELD: supply truck / crate resupply & repair
    Rotate,          // C15ROTAT: face armor direction
    TowGun,          // C16PCKUG: hitch / tow artillery (M_PRICEP)
    Pickup,          // C16PCKUP: board / crew / capture vehicle
    EnterBuilding,   // C17GOLN: garrison building
    AirFly,          // C18FLY: fighter / transport sortie target
    AirBomb,         // C19BOMB: bomber strike target
    AirPara,         // C20PARA: paratrooper drop zone
    AirSpy,          // C21SPY: recon sortie target
    Heal,            // C26CURE: medic / ambulance heal
    Mine,            // C27MINE: sapper mine disarm / lay
    Binoculars,      // C28BINOC: officer / sniper binoculars
    FixBridge,       // C29FIXBR: supply truck bridge repair
    Count
};

class TacticalCursorBank {
public:
    static constexpr const char* kWindowProp = "ss2.tactical_cursor_bank";
    static constexpr std::uint64_t kFrameStepMs = 45u;

    explicit TacticalCursorBank(std::filesystem::path gameDir, SDL_Window* window = nullptr)
        : window_(window) {
        if (!gameDir.empty()) {
            archives_ = assets::resolveGameArchives(gameDir);
            hasArchive_ = !archives_.empty();
        }
    }

    void apply(TacticalCursorKind kind, SDL_Cursor* fallbackMove, SDL_Cursor* fallbackAttack) {
        const auto idx = static_cast<std::size_t>(kind);
        if (idx >= slots_.size()) return;
        const int targetScale = cursor_detail::cursorScaleForWindow(window_);
        auto& slot = slots_[idx];
        if (!slot.attempted || slot.scale != targetScale) {
            slot.attempted = true;
            slot.scale = targetScale;
            loadSlot(kind, slot, targetScale);
        }
        if (!slot.cursors.empty()) {
            const std::size_t frameIdx =
                static_cast<std::size_t>((SDL_GetTicks() / kFrameStepMs) % slot.cursors.size());
            SDL_Cursor* cur = slot.cursors[frameIdx].get();
            if (cur && cur != activeRaw_) {
                SDL_SetCursor(cur);
                activeRaw_ = cur;
            }
            return;
        }
        SDL_Cursor* fb = SDL_GetDefaultCursor();
        switch (kind) {
        case TacticalCursorKind::AttackUnit:
        case TacticalCursorKind::AttackGround:
        case TacticalCursorKind::AttackMove:
        case TacticalCursorKind::KatyushaFire:
        case TacticalCursorKind::HowitzerFire:
        case TacticalCursorKind::AirBomb:
            fb = fallbackAttack ? fallbackAttack : SDL_GetDefaultCursor();
            break;
        case TacticalCursorKind::Field:
        case TacticalCursorKind::OwnUnit:
        case TacticalCursorKind::FriendUnit:
        case TacticalCursorKind::EnemyUnit:
        case TacticalCursorKind::Forbidden:
            fb = SDL_GetDefaultCursor();
            break;
        default:
            fb = fallbackMove ? fallbackMove : SDL_GetDefaultCursor();
            break;
        }
        if (fb && fb != activeRaw_) {
            SDL_SetCursor(fb);
            activeRaw_ = fb;
        }
    }

    void resetToField(SDL_Cursor* fallbackMove, SDL_Cursor* fallbackAttack) {
        activeRaw_ = nullptr;
        apply(TacticalCursorKind::Field, fallbackMove, fallbackAttack);
    }

private:
    using CursorPtr = std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)>;
    struct Slot {
        bool attempted{false};
        int scale{1};
        std::vector<CursorPtr> cursors;
    };

    static std::string_view stemForKind(TacticalCursorKind kind) {
        switch (kind) {
        case TacticalCursorKind::Field: return "C1FIELD";
        case TacticalCursorKind::OwnUnit: return "C2OWN";
        case TacticalCursorKind::FriendUnit: return "C3FRIEND";
        case TacticalCursorKind::EnemyUnit: return "C4ENEMY";
        case TacticalCursorKind::Forbidden: return "C6NO";
        case TacticalCursorKind::Unload: return "C7UNLOAD";
        case TacticalCursorKind::Move: return "C8MOVE";
        case TacticalCursorKind::AttackUnit: return "C9ATTACK";
        case TacticalCursorKind::AttackGround: return "C10AGRND";
        case TacticalCursorKind::AttackMove: return "C11AMOVE";
        case TacticalCursorKind::KatyushaFire: return "C12KATFR";
        case TacticalCursorKind::HowitzerFire: return "C13HOWFR";
        case TacticalCursorKind::Reload: return "C14RELD";
        case TacticalCursorKind::Rotate: return "C15ROTAT";
        case TacticalCursorKind::TowGun: return "C16PCKUG";
        case TacticalCursorKind::Pickup: return "C16PCKUP";
        case TacticalCursorKind::EnterBuilding: return "C17GOLN";
        case TacticalCursorKind::AirFly: return "C18FLY";
        case TacticalCursorKind::AirBomb: return "C19BOMB";
        case TacticalCursorKind::AirPara: return "C20PARA";
        case TacticalCursorKind::AirSpy: return "C21SPY";
        case TacticalCursorKind::Heal: return "C26CURE";
        case TacticalCursorKind::Mine: return "C27MINE";
        case TacticalCursorKind::Binoculars: return "C28BINOC";
        case TacticalCursorKind::FixBridge: return "C29FIXBR";
        case TacticalCursorKind::Count: break;
        }
        return "C1FIELD";
    }

    void loadSlot(TacticalCursorKind kind, Slot& slot, int scaleFactor) {
        slot.cursors.clear();
        if (!hasArchive_) return;
        const auto stem = std::string(stemForKind(kind));
        slot.cursors = cursor_detail::buildCursorsFromChain(
            archives_, stem + ".ANI", stem + ".COL", scaleFactor);
    }

    SDL_Window* window_{nullptr};
    std::vector<std::filesystem::path> archives_;
    bool hasArchive_{false};
    std::array<Slot, static_cast<std::size_t>(TacticalCursorKind::Count)> slots_{};
    SDL_Cursor* activeRaw_{nullptr};
};

inline void SDLCALL destroyTacticalCursorBankProp(void* /*userdata*/, void* value) {
    SDL_SetCursor(SDL_GetDefaultCursor());
    delete static_cast<TacticalCursorBank*>(value);
}

inline void installTacticalCursorBank(SDL_Window* window, const std::filesystem::path& gameDir) {
    if (!window) return;
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props) return;
    auto* bank = new TacticalCursorBank(gameDir, window);
    if (!SDL_SetPointerPropertyWithCleanup(props, TacticalCursorBank::kWindowProp, bank,
                                           destroyTacticalCursorBankProp, nullptr)) {
        delete bank;
    }
}

inline TacticalCursorBank* getTacticalCursorBank(SDL_Window* window) {
    if (!window) return nullptr;
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props) return nullptr;
    return static_cast<TacticalCursorBank*>(
        SDL_GetPointerProperty(props, TacticalCursorBank::kWindowProp, nullptr));
}

inline void applyTacticalCursor(SDL_Window* window, TacticalCursorKind kind,
                                SDL_Cursor* fallbackMove = nullptr,
                                SDL_Cursor* fallbackAttack = nullptr) {
    if (auto* bank = getTacticalCursorBank(window)) {
        bank->apply(kind, fallbackMove, fallbackAttack);
        return;
    }
    SDL_Cursor* fb = SDL_GetDefaultCursor();
    if (kind == TacticalCursorKind::AttackUnit || kind == TacticalCursorKind::AttackGround ||
        kind == TacticalCursorKind::AttackMove || kind == TacticalCursorKind::KatyushaFire ||
        kind == TacticalCursorKind::HowitzerFire || kind == TacticalCursorKind::AirBomb) {
        fb = fallbackAttack ? fallbackAttack : SDL_GetDefaultCursor();
    } else if (kind != TacticalCursorKind::Field && kind != TacticalCursorKind::OwnUnit &&
               kind != TacticalCursorKind::FriendUnit && kind != TacticalCursorKind::EnemyUnit &&
               kind != TacticalCursorKind::Forbidden) {
        fb = fallbackMove ? fallbackMove : SDL_GetDefaultCursor();
    }
    SDL_SetCursor(fb);
}

/// Animated 21-frame retail menu cursor (menu_common.sue\MENU_CUR.ANI/.COL, 50 ms cadence).
class MenuCursorAnimator {
public:
    static constexpr std::uint64_t kFrameStepMs = 50u;

    ~MenuCursorAnimator() {
        restore();
        cursors_.clear();
    }

    void load(const std::filesystem::path& gameDir, SDL_Window* window = nullptr) {
        restore();
        cursors_.clear();
        gameDir_ = gameDir;
        window_ = window;
        loadedScale_ = cursor_detail::cursorScaleForWindow(window_);
        if (gameDir_.empty() || !SDL_WasInit(SDL_INIT_VIDEO)) return;
        const auto menuArchives = assets::resolveMenuArchives(gameDir_);
        if (menuArchives.empty()) return;
        cursors_ = cursor_detail::buildCursorsFromChain(
            menuArchives, "MENU_CUR.ANI", "MENU_CUR.COL", loadedScale_);
    }

    bool loaded() const noexcept { return !cursors_.empty(); }
    std::size_t frameCount() const noexcept { return cursors_.size(); }

    void apply() {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
        const int targetScale = cursor_detail::cursorScaleForWindow(window_);
        if (!gameDir_.empty() && targetScale != loadedScale_) {
            load(gameDir_, window_);
        }
        if (cursors_.empty()) return;
        const std::size_t frameIdx =
            static_cast<std::size_t>((SDL_GetTicks() / kFrameStepMs) % cursors_.size());
        SDL_Cursor* cur = cursors_[frameIdx].get();
        if (cur && cur != activeRaw_) {
            SDL_SetCursor(cur);
            activeRaw_ = cur;
        }
    }

    void restore() {
        if (activeRaw_ != nullptr) {
            if (SDL_WasInit(SDL_INIT_VIDEO)) {
                SDL_SetCursor(SDL_GetDefaultCursor());
            }
            activeRaw_ = nullptr;
        }
    }

private:
    std::filesystem::path gameDir_;
    SDL_Window* window_{nullptr};
    int loadedScale_{1};
    std::vector<cursor_detail::CursorPtr> cursors_;
    SDL_Cursor* activeRaw_{nullptr};
};

} // namespace ss2
