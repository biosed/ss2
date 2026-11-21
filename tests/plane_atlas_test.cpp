#include "render/plane_atlas.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(std::string("plane_atlas_test: ") + what);
}
}

int main() {
    using ss2::PlaneAtlasAllocator;

    // Same sprite key reuses one slot while any plane holds it; freed only after the last release.
    {
        PlaneAtlasAllocator a(16, 4);
        const unsigned s1 = a.acquire("bomber#0");
        const unsigned s2 = a.acquire("bomber#0");
        check(s1 == s2 && s1 >= 16 && s1 < 20, "same key reuses one slot");
        a.release("bomber#0");
        const unsigned s3 = a.acquire("fighter#1");
        check(s3 != s1, "slot not recycled while a plane still holds the key");
        a.release("bomber#0");
        const unsigned s4 = a.acquire("recon#2");
        check(s4 == s1, "slot recycled after the last plane holding the key dies");
    }

    // Distinct live sprites get distinct simultaneous slots up to the live capacity.
    {
        PlaneAtlasAllocator a(16, 4);          // 3 live slots + 1 overflow
        check(a.liveCount() == 3, "live capacity is count-1");
        const unsigned s0 = a.acquire("a#0");
        const unsigned s1 = a.acquire("b#0");
        const unsigned s2 = a.acquire("c#0");
        check(s0 != s1 && s0 != s2 && s1 != s2, "three distinct sprites get three distinct slots");
        check(s0 >= 16 && s2 < 20, "slots stay within the reserved plane range");
        check(!a.exhausted(), "filling the live capacity is not exhaustion");
    }

    // The original bug: a later insertion must not overwrite a slot a live plane still uses.
    {
        PlaneAtlasAllocator a(16, 5);          // 4 live slots + 1 overflow
        const unsigned first = a.acquire("a#0");
        a.acquire("b#0"); a.acquire("c#0"); a.acquire("d#0");
        a.release("b#0");                 // one plane dies, freeing its slot
        const unsigned fifth = a.acquire("e#0");
        check(fifth != first, "fifth sprite takes the freed slot, not the first live plane's slot");
        check(a.acquire("a#0") == first, "first live plane keeps its original slot");
    }

    // Exhaustion degrades to the overflow-only slot, which is never a live tracked slot.
    {
        PlaneAtlasAllocator a(16, 4);          // live slots 16,17,18; overflow 19
        const unsigned s0 = a.acquire("a#0");
        const unsigned s1 = a.acquire("b#0");
        const unsigned s2 = a.acquire("c#0");
        const unsigned over = a.acquire("d#0");
        check(a.exhausted(), "exhaustion flagged when distinct sprites exceed live capacity");
        check(over == a.overflowSlot(), "fallback uses the dedicated overflow slot");
        check(over != s0 && over != s1 && over != s2, "fallback slot is not any live tracked slot");
        check(a.acquire("a#0") == s0 && a.acquire("b#0") == s1 && a.acquire("c#0") == s2,
              "tracked slots survive exhaustion unchanged");
        a.release("d#0");                 // untracked overflow key: must not free a real slot
        const unsigned over2 = a.acquire("e#0");
        check(over2 == a.overflowSlot() && over2 != s0 && over2 != s1 && over2 != s2,
              "repeat overflow still avoids every live tracked slot");
        check(a.acquire("a#0") == s0, "overflow release frees nothing; owner keeps its slot");
    }

    // Release of an unknown key is a no-op; reset refills the free list.
    {
        PlaneAtlasAllocator a(16, 2);          // 1 live slot (16) + overflow (17)
        a.release("never-seen#0");
        const unsigned s0 = a.acquire("a#0");
        const unsigned s1 = a.acquire("b#0");
        check(s1 == a.overflowSlot() && a.exhausted(), "full before reset");
        a.reset();
        check(!a.exhausted(), "reset clears exhaustion");
        check(a.acquire("x#0") == s0 && a.acquire("y#0") == s1, "reset refills the free list");
    }

    std::cout << "plane_atlas_test: all checks passed\n";
    return 0;
}
