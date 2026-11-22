#pragma once
#include "assets/indexed.hpp"
#include "assets/sprite.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace ss2 {

namespace cursor_detail {
using CursorPtr = std::unique_ptr<SDL_Cursor, decltype(&SDL_DestroyCursor)>;

inline std::vector<CursorPtr> buildCursorsFromSue(const std::filesystem::path& archivePath,
                                                  std::string_view aniEntry,
                                                  std::string_view colEntry) {
    std::vector<CursorPtr> out;
    if (archivePath.empty() || !SDL_WasInit(SDL_INIT_VIDEO)) return out;
    try {
        const auto ani = assets::readSueEntry(archivePath, aniEntry);
        const auto col = assets::readSueEntry(archivePath, colEntry);
        auto seq = assets::decodeAniSequence(ani, col);
        if (seq.frames.empty() || seq.width == 0u || seq.height == 0u) return out;
        const int hotX = std::clamp(seq.hotX, 0, int(seq.width) - 1);
        const int hotY = std::clamp(seq.hotY, 0, int(seq.height) - 1);
        out.reserve(seq.frames.size());
        for (auto& frame : seq.frames) {
            SDL_Surface* surf = SDL_CreateSurfaceFrom(
                int(frame.width), int(frame.height), SDL_PIXELFORMAT_RGBA32,
                frame.rgba.data(), int(frame.width) * 4);
            if (!surf) continue;
            SDL_Cursor* cur = SDL_CreateColorCursor(surf, hotX, hotY);
            SDL_DestroySurface(surf);
            if (cur) out.emplace_back(cur, SDL_DestroyCursor);
        }
    } catch (...) {
        out.clear();
    }
    return out;
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

    explicit TacticalCursorBank(std::filesystem::path gameDir) {
        if (!gameDir.empty()) {
            archivePath_ = assets::resolveCaseInsensitivePath(gameDir, "game_common.sue");
            std::error_code ec;
            hasArchive_ = std::filesystem::is_regular_file(archivePath_, ec);
        }
    }

    void apply(TacticalCursorKind kind, SDL_Cursor* fallbackMove, SDL_Cursor* fallbackAttack) {
        const auto idx = static_cast<std::size_t>(kind);
        if (idx >= slots_.size()) return;
        auto& slot = slots_[idx];
        if (!slot.attempted) {
            slot.attempted = true;
            loadSlot(kind, slot);
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

    void loadSlot(TacticalCursorKind kind, Slot& slot) {
        if (!hasArchive_) return;
        const auto stem = std::string(stemForKind(kind));
        slot.cursors = cursor_detail::buildCursorsFromSue(
            archivePath_, stem + ".ANI", stem + ".COL");
    }

    std::filesystem::path archivePath_;
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
    auto* bank = new TacticalCursorBank(gameDir);
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

    void load(const std::filesystem::path& gameDir) {
        restore();
        cursors_.clear();
        if (gameDir.empty() || !SDL_WasInit(SDL_INIT_VIDEO)) return;
        const auto menuSue = assets::resolveCaseInsensitivePath(gameDir, "menu_common.sue");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(menuSue, ec)) return;
        cursors_ = cursor_detail::buildCursorsFromSue(menuSue, "MENU_CUR.ANI", "MENU_CUR.COL");
    }

    bool loaded() const noexcept { return !cursors_.empty(); }
    std::size_t frameCount() const noexcept { return cursors_.size(); }

    void apply() {
        if (cursors_.empty() || !SDL_WasInit(SDL_INIT_VIDEO)) return;
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
    std::vector<cursor_detail::CursorPtr> cursors_;
    SDL_Cursor* activeRaw_{nullptr};
};

} // namespace ss2
