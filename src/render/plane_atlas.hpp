#ifndef SS2_RENDER_PLANE_ATLAS_HPP
#define SS2_RENDER_PLANE_ATLAS_HPP

#include <map>
#include <string>
#include <vector>

namespace ss2 {

// Assigns GPU sprite-atlas slots to live plane sprites. Planes are drawn in a single
// batched pass, so every distinct sprite alive at once needs its own slot simultaneously.
// Slots are keyed by sprite (planeId + direction): planes sharing a sprite share a slot,
// and a slot is recycled only when its last live plane dies. This replaces a rotating
// cursor that overwrote slots still referenced by earlier live planes.
//
// The range [base, base+count) is split into count-1 live slots plus one overflow-only
// slot at the top. The overflow slot is never handed to a tracked key, so when more
// distinct sprites are live than there are live slots, the fallback can degrade the
// overflow plane's own image without corrupting any live plane's slot.
class PlaneAtlasAllocator {
public:
    PlaneAtlasAllocator(unsigned base, unsigned count) : base_(base), count_(count) {
        reset();
    }

    // Returns the slot for a sprite key and counts one more live plane using it.
    unsigned acquire(const std::string& key) {
        ++refs_[key];
        const auto it = slots_.find(key);
        if (it != slots_.end()) return it->second;
        if (!free_.empty()) {
            const unsigned slot = free_.back();
            free_.pop_back();
            slots_.emplace(key, slot);
            exhausted_ = false;
            return slot;
        }
        // More distinct live sprites than live slots: degrade to the overflow-only slot.
        // It is never tracked, so no live plane's slot is written and release() frees nothing.
        exhausted_ = true;
        return overflowSlot();
    }

    // Drops one live plane's claim; frees the slot when no live plane uses the key.
    void release(const std::string& key) {
        const auto rit = refs_.find(key);
        if (rit == refs_.end()) return;
        if (--rit->second > 0) return;
        refs_.erase(rit);
        const auto sit = slots_.find(key);
        if (sit != slots_.end()) {
            free_.push_back(sit->second);
            slots_.erase(sit);
        }
    }

    void reset() {
        slots_.clear();
        refs_.clear();
        free_.clear();
        for (unsigned i = 0; i + 1 < count_; ++i) free_.push_back(base_ + i);
        exhausted_ = false;
    }

    unsigned base() const { return base_; }
    unsigned count() const { return count_; }
    unsigned liveCount() const { return count_ > 0 ? count_ - 1 : 0; }
    unsigned overflowSlot() const { return base_ + (count_ > 0 ? count_ - 1 : 0); }
    bool exhausted() const { return exhausted_; }

private:
    unsigned base_;
    unsigned count_;
    std::map<std::string, unsigned> slots_;
    std::map<std::string, unsigned> refs_;
    std::vector<unsigned> free_;
    bool exhausted_ = false;
};

}

#endif
