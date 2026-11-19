#include "assets/sound_bank.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
void pushU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(std::uint8_t(v & 0xffu));
    out.push_back(std::uint8_t((v >> 8) & 0xffu));
    out.push_back(std::uint8_t((v >> 16) & 0xffu));
    out.push_back(std::uint8_t((v >> 24) & 0xffu));
}

void testSyntheticBank() {
    std::vector<std::uint8_t> hdr;
    pushU32(hdr, 1); // 1 group
    char name[64]{};
    std::strncpy(name, "rifle", sizeof(name) - 1);
    hdr.insert(hdr.end(), name, name + 64);
    pushU32(hdr, 1); // 1 clip index
    pushU32(hdr, 0); // index 0

    std::vector<std::uint8_t> rus;
    pushU32(rus, 1);  // 1 clip
    pushU32(rus, 12); // start offset
    pushU32(rus, 28); // end offset (12 hdr + 4 pcm bytes)
    pushU32(rus, 22050);
    pushU32(rus, 16);
    pushU32(rus, 0);
    rus.insert(rus.end(), {0x01, 0x02, 0x03, 0x04});

    const auto bank = ss2::assets::SoundBank::fromMemory(hdr, rus);
    assert(bank.groupCount() == 1);
    assert(bank.clipCount() == 1);
    assert(bank.variantCount("RIFLE") == 1);
    const auto* c = bank.clip("Rifle", 0);
    assert(c != nullptr);
    assert(c->sampleRate == 22050 && c->bitsPerSample == 16 && !c->loop);
    assert(c->pcm.size() == 4 && c->pcm[0] == 0x01 && c->pcm[3] == 0x04);
    assert(bank.clip("nonexistent") == nullptr);
}
} // namespace

int main(int argc, char** argv) {
    testSyntheticBank();
    if (argc > 1) {
        const auto bank = ss2::assets::SoundBank::load(argv[1]);
        assert(bank.groupCount() == 330);
        assert(bank.clipCount() == 693);
        const auto* rifle = bank.clip("rifle", 0);
        assert(rifle && rifle->sampleRate == 22050 && rifle->bitsPerSample == 16 && !rifle->pcm.empty());
        const auto* move = bank.clip("f_sold_move", 0);
        assert(move && !move->pcm.empty());
        const auto* attack = bank.clip("f_sold_attack", 0);
        assert(attack && !attack->pcm.empty());
        std::cout << "gold_sound_bank: PASS (" << bank.groupCount() << " groups, "
                  << bank.clipCount() << " clips)\n";
        return 0;
    }
    std::cout << "sound_bank: PASS (synthetic)\n";
    return 0;
}
