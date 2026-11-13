#pragma once

// Pure C++. Which instance owns a process-wide resource, when instances overlap. The Expo module
// is created once per JS runtime, and on a reload the new module's OnCreate can run before the old
// module's OnDestroy; the old module's teardown (clear the MIDI input handler, stop the beat timer,
// shut the engine down) must then not undo what the new module set up. Each owner claims a token;
// a release with a token that is no longer current does nothing. Harness case "own".

#include <atomic>
#include <cstdint>

class OwnerToken {
public:
    // Any thread. The new owner's token (never 0); every earlier token stops being current.
    uint64_t claim() {
        const uint64_t token = next.fetch_add(1, std::memory_order_relaxed) + 1;
        owner.store(token, std::memory_order_release);
        return token;
    }

    // Any thread. True while `token` is the latest claim and was not released.
    bool isCurrent(uint64_t token) const {
        return token != 0 && owner.load(std::memory_order_acquire) == token;
    }

    // Any thread. Ends `token`'s ownership if it still owns; false (nothing changed) if a later
    // claim took over.
    bool release(uint64_t token) {
        uint64_t expected = token;
        return token != 0 && owner.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }

    // The current owner, 0 = none.
    uint64_t current() const { return owner.load(std::memory_order_acquire); }

private:
    std::atomic<uint64_t> next{0};
    std::atomic<uint64_t> owner{0};
};
