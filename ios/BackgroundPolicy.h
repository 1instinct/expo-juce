#pragma once

// Pure C++: the background policy's decisions (0.9.0-rc.5). ExpoJuceAudioEngine.mm applies them on
// the main thread; the offline harness tests them (case "bg").
//
// The host app runs with the audio background mode, so iOS keeps it alive while its audio session
// is active. The policy keeps the engine running in the background only while it has something to
// play, and lets iOS suspend the app otherwise:
//  - Not set (setBackgroundPolicy never called since the app's JS started): nothing changes when
//    the app goes to the background (the engine's behaviour before 0.9.0-rc.5).
//  - keepAlive: in the background the engine keeps running while it is busy (the transport plays,
//    a voice or drum sounds, the sequence or the MIDI input holds a note, the arpeggiator runs).
//  - keepAlive off: going to the background stops the transport and releases every note (what
//    plays stops when the app leaves the screen).
//  - Either way, once it has been idle (not busy) in the background for idleStopSeconds, the
//    engine stops its audio device and deactivates the session ("idleStopped"), and iOS may
//    suspend the app. Returning to the foreground restarts the device.

#include <algorithm>
#include <cmath>

namespace bgpolicy {

constexpr double kDefaultIdleStopSeconds = 30.0;
constexpr double kMinIdleStopSeconds = 1.0;
constexpr double kMaxIdleStopSeconds = 3600.0;

// Seconds; non-finite → the default, clamped to [kMinIdleStopSeconds, kMaxIdleStopSeconds].
inline double sanitizeIdleStopSeconds(double s) {
    if (!std::isfinite(s)) return kDefaultIdleStopSeconds;
    return std::clamp(s, kMinIdleStopSeconds, kMaxIdleStopSeconds);
}

struct Policy {
    bool set = false; // setBackgroundPolicy called (by the current JS runtime)
    bool keepAlive = true;
    double idleStopSeconds = kDefaultIdleStopSeconds;
};

enum class State { Foreground = 0, Background = 1, IdleStopped = 2 };

inline const char* stateName(State s) {
    switch (s) {
        case State::Background: return "background";
        case State::IdleStopped: return "idleStopped";
        default: return "foreground";
    }
}

// What the engine is doing, sampled on each check.
struct Activity {
    bool transportPlaying = false;
    int voicesSounding = 0;  // synth voices (any stage) and drum voices
    int sequenceNotes = 0;   // notes the sequence player holds
    int inputHolds = 0;      // notes the MIDI byte input holds (a device key held down)
    bool arpRunning = false; // the arpeggiator has notes (held or latched)
    bool toneOrOther = false; // anything else that must not be cut (test tone)
};

inline bool isBusy(const Activity& a) {
    return a.transportPlaying || a.voicesSounding > 0 || a.sequenceNotes > 0 || a.inputHolds > 0 || a.arpRunning
        || a.toneOrOther;
}

// The idle timer. Times are seconds on any monotonic clock. Main thread only (no atomics).
class IdleTracker {
public:
    State state() const { return current; }
    // The time the idle stop is due, or -1 when none is pending (foreground, busy, policy unset,
    // already stopped).
    double idleStopAt() const { return stopAt; }

    // The app went to the background. Returns true when the engine should pause now (policy set
    // with keepAlive off: stop the transport, release every note).
    bool enterBackground(const Policy& p, double now) {
        if (current == State::Foreground) current = State::Background;
        idleSince = now;
        stopAt = -1.0;
        return p.set && !p.keepAlive;
    }

    // The app came back. Returns true when the device must be restarted (it was idle-stopped).
    bool enterForeground() {
        const bool restart = current == State::IdleStopped;
        current = State::Foreground;
        stopAt = -1.0;
        return restart;
    }

    // A periodic check in the background. Returns true when the engine should stop its device now
    // (the state is then IdleStopped until markRestarted / enterForeground).
    bool check(const Policy& p, const Activity& a, double now) {
        if (current != State::Background || !p.set) {
            stopAt = -1.0;
            return false;
        }
        if (isBusy(a)) idleSince = now;
        const double idle = sanitizeIdleStopSeconds(p.idleStopSeconds);
        stopAt = idleSince + idle;
        if (now >= stopAt) {
            current = State::IdleStopped;
            stopAt = -1.0;
            return true;
        }
        return false;
    }

    // The device was restarted while the app stayed in the background (a remote play command):
    // back to Background, the idle timer starting now.
    void markRestarted(double now) {
        if (current == State::IdleStopped) current = State::Background;
        idleSince = now;
        stopAt = -1.0;
    }

private:
    State current = State::Foreground;
    double idleSince = 0.0;
    double stopAt = -1.0;
};

// Whether the session should be non-mixable (other apps' audio pauses), which iOS requires to
// show the app's Now Playing info and route the lock-screen controls to it: only while Now Playing
// info is set, and the transport plays or the app is in the background (so a lock-screen pause can
// be followed by a play).
inline bool wantsExclusiveSession(bool nowPlayingSet, bool transportPlaying, bool inBackground) {
    return nowPlayingSet && (transportPlaying || inBackground);
}

} // namespace bgpolicy
