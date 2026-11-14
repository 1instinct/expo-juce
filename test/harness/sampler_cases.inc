// Sampler cases (0.10.0): included by harness.cpp after its helpers (allocation counter, writeWav,
// rmsOf, lastNonZero). Case ids: sm (voice core), smb (32-voice budget), sml (loader), sms (swap
// stress), smq (through PolySynthProcessor + note queue), smd (src/samplerDefaults.ts).

namespace smtest {

// A sample held by the test (the engine only borrows its pointers).
struct TestSample {
    std::vector<float> L, R;
    SampleBuffer view{};
};

static std::unique_ptr<TestSample> makeSample(int frames, double rate, const std::function<float(int)>& fL,
                                              const std::function<float(int)>& fR = {}) {
    auto s = std::make_unique<TestSample>();
    s->L.resize((size_t)frames);
    s->R.resize((size_t)frames);
    for (int i = 0; i < frames; ++i) {
        s->L[(size_t)i] = fL(i);
        s->R[(size_t)i] = fR ? fR(i) : fL(i);
    }
    s->view = { s->L.data(), s->R.data(), frames, rate };
    return s;
}

// Deterministic noise-ish signal in [-0.5, 0.5].
static float noise(int i, uint32_t salt = 1) {
    uint32_t x = (uint32_t)i * 2654435761u ^ (salt * 0x9e3779b9u);
    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
    return (float)((double)(x & 0xffff) / 65535.0 - 0.5);
}

// Prepared at `sr` with the engine level at 1 (its default is 0.8), so outputs equal the formulas.
static void prep(SamplerEngine& e, double sr) {
    e.prepare(sr, 512);
    e.setLevel(1.0f);
}

static SamplerSlotParams plain() {
    SamplerSlotParams p;
    p.attackMs = 0.0f;
    return p;
}

// Renders `total` frames in `block`-sized calls (allocation counter on), appending to outL / outR.
// `before(done)` runs before each block (hits, note-ons) with the counter on too.
static void render(SamplerEngine& e, int total, int block, std::vector<float>& outL, std::vector<float>& outR,
                   const std::function<void(int)>& before = {}) {
    std::vector<float> l((size_t)block), r((size_t)block);
    for (int done = 0; done < total;) {
        const int n = std::min(block, total - done);
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        tCountAllocs = true;
        if (before) before(done);
        e.render(l.data(), r.data(), n);
        tCountAllocs = false;
        outL.insert(outL.end(), l.begin(), l.begin() + n);
        outR.insert(outR.end(), r.begin(), r.begin() + n);
        done += n;
    }
}

static float maxStep(const std::vector<float>& x, int from, int to) {
    float m = 0.0f;
    for (int i = std::max(1, from); i < std::min((int)x.size(), to); ++i)
        m = std::max(m, std::fabs(x[(size_t)i] - x[(size_t)i - 1]));
    return m;
}

static int soundingWithNote(const SamplerEngine& e, int note, bool includeReleasing) {
    int n = 0;
    for (int v = 0; v < kSamplerVoices; ++v) {
        const SamplerVoiceInfo info = e.voiceInfo(v);
        if (info.active && info.note == note && (includeReleasing || !info.releasing)) ++n;
    }
    return n;
}

struct Checks {
    int failed = 0;
    std::string detail;
    void operator()(bool ok, const std::string& what) {
        if (!ok) { ++failed; if (detail.size() < 1500) detail += " [" + what + "]"; }
    }
};

// One named sub-case: prints "  ok   name" or "  FAIL name: ..." and returns whether it passed.
static bool report(const char* name, const Checks& c, const std::string& note = {}) {
    std::printf("  %-4s %s%s%s%s\n", c.failed == 0 ? "ok" : "FAIL", name, note.empty() ? "" : ": ", note.c_str(),
                c.detail.c_str());
    std::fflush(stdout);
    return c.failed == 0;
}

} // namespace smtest

// ── sm: the voice core (ios/SamplerEngine.h) ─────────────────────────────────────────────────

static bool runSamplerCore() {
    using namespace smtest;
    const double sr = 48000.0;
    int failedCases = 0;
    gRenderAllocs.store(0); gRenderFrees.store(0);
    std::printf("sm    sampler voice core (ios/SamplerEngine.h)\n");

    // sm_hit_root_plays_identity: a hit at the root, unity gain, no attack, plays the buffer
    // sample for sample on both channels, then exact zeros and the voice ends.
    {
        Checks c;
        auto s = makeSample(1000, sr, [](int i) { return noise(i, 1); }, [](int i) { return noise(i, 2); });
        SamplerEngine e;
        prep(e, sr);
        e.setSlotBuffer(3, &s->view);
        c(e.setSlotParams(3, plain()), "setSlotParams 3");
        std::vector<float> l, r;
        render(e, 1300, 128, l, r, [&](int done) { if (done == 0) e.hit(3, 127); });
        int wrong = 0;
        for (int i = 0; i < 1000; ++i) wrong += (l[(size_t)i] != s->L[(size_t)i] || r[(size_t)i] != s->R[(size_t)i]) ? 1 : 0;
        int tail = 0;
        for (int i = 1000; i < 1300; ++i) tail += (l[(size_t)i] != 0.0f || r[(size_t)i] != 0.0f) ? 1 : 0;
        c(wrong == 0, std::to_string(wrong) + " samples differ from the buffer");
        c(tail == 0, std::to_string(tail) + " non-zero samples after the end");
        c(e.activeVoiceCount() == 0, "voice ends at the buffer's end");
        c(e.diag.hits.load() == 1, "hit counted");
        failedCases += report("sm_hit_root_plays_identity", c, std::to_string(wrong) + " of 1000 differ") ? 0 : 1;
    }

    // sm_pitch_octave_halves_length: +12 halves the length, -12 doubles it, tune +12 on a hit
    // halves it, a 24 kHz buffer on a 48 kHz engine plays at its own speed (twice the frames).
    {
        Checks c;
        auto s = makeSample(4800, sr, [](int) { return 0.5f; });
        auto half = makeSample(2400, sr / 2.0, [](int) { return 0.5f; });
        auto lengthOf = [&](const std::function<void(SamplerEngine&)>& start, const SampleBuffer* buf,
                            const SamplerSlotParams& p) {
            SamplerEngine e;
            prep(e, sr);
            e.setSlotBuffer(0, buf);
            e.setSlotParams(0, p);
            std::vector<float> l, r;
            render(e, 12000, 128, l, r, [&](int done) { if (done == 0) start(e); });
            return lastNonZero(l);
        };
        const int up = lengthOf([](SamplerEngine& e) { e.noteOn(72, 127); }, &s->view, plain());
        const int down = lengthOf([](SamplerEngine& e) { e.noteOn(48, 127); }, &s->view, plain());
        SamplerSlotParams tuned = plain();
        tuned.tuneSemis = 12.0f;
        const int tunedLen = lengthOf([](SamplerEngine& e) { e.hit(0, 127); }, &s->view, tuned);
        SamplerSlotParams cents = plain();
        cents.tuneSemis = 11.0f;
        cents.tuneCents = 100.0f;
        const int centsLen = lengthOf([](SamplerEngine& e) { e.hit(0, 127); }, &s->view, cents);
        const int rate = lengthOf([](SamplerEngine& e) { e.hit(0, 127); }, &half->view, plain());
        c(std::abs(up - 2400) <= 2, "+12: " + std::to_string(up) + " frames (want 2400)");
        c(std::abs(down - 9600) <= 2, "-12: " + std::to_string(down) + " frames (want 9600)");
        c(std::abs(tunedLen - 2400) <= 2, "tune +12: " + std::to_string(tunedLen));
        c(std::abs(centsLen - 2400) <= 2, "tune 11 + 100 cents: " + std::to_string(centsLen));
        c(std::abs(rate - 4800) <= 2, "24 kHz buffer at 48 kHz: " + std::to_string(rate) + " (want 4800)");
        char note[96];
        std::snprintf(note, sizeof(note), "+12 %d, -12 %d, tune %d, 24k buffer %d frames", up, down, tunedLen, rate);
        failedCases += report("sm_pitch_octave_halves_length", c, note) ? 0 : 1;
    }

    // sm_oneshot_ignores_noteoff: a one-shot plays to its end after a note-off or a slot release.
    {
        Checks c;
        auto s = makeSample(6000, sr, [](int) { return 0.5f; });
        SamplerEngine e;
        prep(e, sr);
        e.setSlotBuffer(0, &s->view);
        e.setSlotParams(0, plain()); // oneShot by default
        std::vector<float> l, r;
        render(e, 7000, 128, l, r, [&](int done) {
            if (done == 0) { e.noteOn(60, 127); e.hit(0, 127); }
            if (done == 256) { e.noteOff(60); e.release(0); }
        });
        c(lastNonZero(l) == 6000, "plays to the end: last sound at " + std::to_string(lastNonZero(l)));
        c(std::fabs(l[5990] - 1.0f) < 1e-6f, "both voices still at full level near the end: " + std::to_string(l[5990]));
        failedCases += report("sm_oneshot_ignores_noteoff", c) ? 0 : 1;
    }

    // sm_gated_release_ramps: a gated slot holds while the key is down, then ramps down
    // monotonically over releaseMs to exact zero and ends; a gated pad hit ends on release(slot).
    {
        Checks c;
        auto s = makeSample(48000, sr, [](int) { return 0.5f; });
        SamplerEngine e;
        prep(e, sr);
        e.setSlotBuffer(0, &s->view);
        SamplerSlotParams p = plain();
        p.oneShot = false;
        p.releaseMs = 10.0f;
        e.setSlotParams(0, p);
        std::vector<float> l, r;
        render(e, 4000, 128, l, r, [&](int done) {
            if (done == 0) e.noteOn(60, 127);
            if (done == 1024) e.noteOff(60);
        });
        bool held = true;
        for (int i = 0; i < 1024; ++i) held = held && l[(size_t)i] == 0.5f;
        bool monotonic = true;
        for (int i = 1025; i < 4000; ++i) monotonic = monotonic && l[(size_t)i] <= l[(size_t)i - 1];
        const int end = lastNonZero(l);
        c(held, "holds at 0.5 while the key is down");
        c(monotonic, "release never rises");
        c(end > 1024 + 400 && end <= 1024 + 480 + 1, "reaches 0 after 10 ms: last sound at " + std::to_string(end));
        c(e.activeVoiceCount() == 0, "voice ends after its release");
        // Pad: a gated hit sounds until release(slot).
        std::vector<float> l2, r2;
        render(e, 2000, 128, l2, r2, [&](int done) {
            if (done == 0) e.hit(0, 127);
            if (done == 512) e.release(0);
        });
        const int end2 = lastNonZero(l2);
        c(l2[500] == 0.5f && end2 > 512 + 400 && end2 <= 512 + 481, "gated hit ends on release(slot): " + std::to_string(end2));
        failedCases += report("sm_gated_release_ramps", c, "last sound " + std::to_string(end - 1024) + " frames after the note-off") ? 0 : 1;
    }

    // sm_loop_crossfade_no_click: a 440 Hz sine looped over a region that is not a whole number of
    // periods (the raw seam jumps by more than 0.2) plays past the buffer's end with no step larger
    // than the sine's own; same for a loop over the whole buffer (loop start at 0).
    {
        Checks c;
        const float amp = 0.5f;
        auto s = makeSample(48000, sr, [&](int i) { return amp * (float)std::sin(2.0 * M_PI * 440.0 * i / sr); });
        const float ownStep = amp * (float)(2.0 * M_PI * 440.0 / sr);
        // During the crossfade the blend of two phases of the sine moves by at most its own step
        // plus the fade's slope times their largest difference (2 amp over the crossfade's frames).
        const float allowed = (ownStep + 2.0f * amp / (float)(kSamplerLoopCrossfadeMs * sr / 1000.0)) * 1.01f;
        auto run = [&](float loopStartMs, float loopEndMs, float& seam, float& step, double& lateRms) {
            SamplerEngine e;
            prep(e, sr);
            e.setSlotBuffer(0, &s->view);
            SamplerSlotParams p = plain();
            p.oneShot = false;
            p.loop = true;
            p.loopStartMs = loopStartMs;
            p.loopEndMs = loopEndMs;
            e.setSlotParams(0, p);
            const int ls = (int)std::lround(loopStartMs * sr / 1000.0);
            const int le = loopEndMs < 0 ? 48000 : (int)std::lround(loopEndMs * sr / 1000.0);
            seam = le < 48000 ? std::fabs(s->L[(size_t)le] - s->L[(size_t)ls]) : 0.0f; // the jump if played as is
            std::vector<float> l, r;
            render(e, 96000, 128, l, r, [&](int done) { if (done == 0) e.noteOn(60, 127); });
            step = maxStep(l, 1, 96000);
            lateRms = rmsOf(l, 72000, 96000);
        };
        float seamA = 0, stepA = 0, seamB = 0, stepB = 0;
        double rmsA = 0, rmsB = 0;
        run(100.0f, 337.3f, seamA, stepA, rmsA);
        run(0.0f, -1.0f, seamB, stepB, rmsB);
        // The seam of the whole buffer: x[47999] vs x[0] for 440 Hz over 1 s is a whole number of
        // periods, so only region A is a real discontinuity; B checks the start-at-0 path.
        c(seamA > 0.2f, "region A raw seam is a real jump: " + std::to_string(seamA));
        c(stepA <= allowed, "region A largest step " + std::to_string(stepA) + " vs allowed " + std::to_string(allowed));
        c(stepB <= allowed, "whole-buffer loop largest step " + std::to_string(stepB));
        c(rmsA > 0.3 && rmsB > 0.3, "still sounding after the buffer's length: rms " + std::to_string(rmsA) + " / " + std::to_string(rmsB));
        char note[128];
        std::snprintf(note, sizeof(note), "raw seam %.3f, largest step %.4f (sine's own %.4f, allowed %.4f), whole-buffer loop %.4f",
                      seamA, stepA, ownStep, allowed, stepB);
        failedCases += report("sm_loop_crossfade_no_click", c, note) ? 0 : 1;
    }

    // sm_choke_group_cuts_previous: slot 1 (group 1) chokes a sounding slot 0 (group 1) within
    // the choke fade; group 0 chokes nothing; one note-on starting two slots of one group keeps both.
    {
        Checks c;
        auto open = makeSample(48000, sr, [](int) { return 0.25f; });
        auto closed = makeSample(48000, sr, [](int) { return 0.125f; });
        SamplerEngine e;
        prep(e, sr);
        e.setSlotBuffer(0, &open->view);
        e.setSlotBuffer(1, &closed->view);
        e.setSlotBuffer(2, &closed->view);
        SamplerSlotParams p = plain();
        p.chokeGroup = 1;
        p.lowNote = 40; p.highNote = 40;
        e.setSlotParams(0, p);
        e.setSlotParams(1, p);
        SamplerSlotParams free = plain();
        free.lowNote = 41; free.highNote = 41;
        e.setSlotParams(2, free);
        std::vector<float> l, r;
        render(e, 2048, 128, l, r, [&](int done) {
            if (done == 0) e.hit(0, 127);
            if (done == 1024) e.hit(1, 127);
        });
        c(l[1000] == 0.25f, "open sounds alone first");
        const float after = l[1024 + 300]; // 6.25 ms after the choke
        c(std::fabs(after - 0.125f) < 1e-6f, "6 ms after the choke only the closed sample: " + std::to_string(after));
        // 10 frames into the choke the open sample is still almost all there (a fade, not a cut).
        c(l[1034] > 0.125f + 0.25f * 0.9f, "the choke fades: " + std::to_string(l[1034]));
        c(e.activeVoiceCount() == 1, "one voice after the choke: " + std::to_string(e.activeVoiceCount()));
        // Group 0 chokes nothing.
        std::vector<float> l2, r2;
        render(e, 512, 128, l2, r2, [&](int done) { if (done == 0) e.hit(2, 127); });
        c(e.activeVoiceCount() == 2, "group 0 leaves the group-1 voice: " + std::to_string(e.activeVoiceCount()));
        // A note-on that starts slots 0 and 1 (both group 1, both zone 40) keeps both.
        SamplerEngine e2;
        prep(e2, sr);
        e2.setSlotBuffer(0, &open->view);
        e2.setSlotBuffer(1, &closed->view);
        e2.setSlotParams(0, p);
        e2.setSlotParams(1, p);
        std::vector<float> l3, r3;
        render(e2, 1024, 128, l3, r3, [&](int done) { if (done == 0) e2.noteOn(40, 127); });
        c(e2.activeVoiceCount() == 2 && l3[900] == 0.375f, "one note-on, two slots of a group: both sound");
        failedCases += report("sm_choke_group_cuts_previous", c) ? 0 : 1;
    }

    // sm_steal_oldest_released_first: polyphony 4, notes 60..63 held, 61 released (long release):
    // the 5th note takes 61's voice; with all held, the next takes the oldest (60). Declicked.
    {
        Checks c;
        auto s = makeSample(96000, sr, [](int) { return 0.1f; });
        SamplerEngine e;
        prep(e, sr);
        e.setPolyphony(4);
        e.setSlotBuffer(0, &s->view);
        SamplerSlotParams p = plain();
        p.oneShot = false;
        p.releaseMs = 1000.0f;
        e.setSlotParams(0, p);
        std::vector<float> l, r;
        render(e, 128 * 8, 128, l, r, [&](int done) {
            const int b = done / 128;
            if (b < 4) e.noteOn(60 + b, 127);
            if (b == 4) e.noteOff(61);
        });
        c(e.activeVoiceCount() == 4 && soundingWithNote(e, 61, true) == 1, "4 voices, 61 releasing");
        render(e, 128, 128, l, r, [&](int) { e.noteOn(64, 127); });
        c(soundingWithNote(e, 61, true) == 0, "the released voice (61) was stolen");
        c(soundingWithNote(e, 60, false) == 1 && soundingWithNote(e, 64, false) == 1, "60 kept, 64 sounds");
        render(e, 128, 128, l, r, [&](int) { e.noteOn(65, 127); });
        c(soundingWithNote(e, 60, true) == 0, "with every voice held, the oldest (60) was stolen");
        c(soundingWithNote(e, 62, false) == 1 && soundingWithNote(e, 63, false) == 1 && soundingWithNote(e, 65, false) == 1,
          "62, 63, 65 sound");
        c(e.activeVoiceCount() == 4, "still 4 voices: " + std::to_string(e.activeVoiceCount()));
        c(e.diag.steals.load() == 2, "2 steals counted: " + std::to_string(e.diag.steals.load()));
        // Each steal starts a new voice at 0.1 while the stolen one fades from its last value over
        // the declick: the sum starts near 0.5 (3 voices + new + fading) and settles at 0.4.
        for (int at : {128 * 8, 128 * 9}) {
            c(l[(size_t)at] > 0.48f && std::fabs(l[(size_t)at + 100] - 0.4f) < 1e-5f,
              "steal at " + std::to_string(at) + " declicked: " + std::to_string(l[(size_t)at]) + " -> " +
              std::to_string(l[(size_t)at + 100]));
        }
        failedCases += report("sm_steal_oldest_released_first", c) ? 0 : 1;
    }

    // sm_zone_maps_note_to_slots: a note-on plays every loaded slot whose zone holds it; an empty
    // zone (low > high) and an unloaded slot never play; out-of-range input is ignored and counted.
    {
        Checks c;
        auto s = makeSample(48000, sr, [](int) { return 0.1f; });
        SamplerEngine e;
        prep(e, sr);
        auto zone = [&](int slot, int lo, int hi, bool load) {
            if (load) e.setSlotBuffer(slot, &s->view);
            SamplerSlotParams p = plain();
            p.lowNote = lo; p.highNote = hi;
            e.setSlotParams(slot, p);
        };
        zone(0, 36, 47, true);
        zone(1, 48, 59, true);
        zone(2, 40, 50, true);
        zone(3, 50, 40, true);   // empty zone: pads only
        zone(4, 0, 127, false);  // not loaded
        auto slotsFor = [&](int note) {
            std::vector<float> l, r;
            render(e, 128, 128, l, r, [&](int) { e.noteOn(note, 100); });
            std::vector<int> slots;
            for (int v = 0; v < kSamplerVoices; ++v) {
                const SamplerVoiceInfo info = e.voiceInfo(v);
                if (info.active && info.note == note) slots.push_back(info.slot);
            }
            std::sort(slots.begin(), slots.end());
            return slots;
        };
        c(slotsFor(45) == std::vector<int>{0, 2}, "45 -> slots 0, 2");
        c(slotsFor(55) == std::vector<int>{1}, "55 -> slot 1");
        c(slotsFor(48) == std::vector<int>{1, 2}, "48 -> slots 1, 2");
        c(slotsFor(30).empty(), "30 -> nothing");
        const uint64_t ignoredBefore = e.diag.ignored.load();
        e.hit(16, 100);
        e.hit(-1, 100);
        e.noteOn(128, 100);
        e.noteOn(-1, 100);
        c(e.diag.ignored.load() - ignoredBefore == 4, "4 out-of-range calls ignored and counted: " +
          std::to_string(e.diag.ignored.load() - ignoredBefore));
        const int before = e.activeVoiceCount();
        e.hit(4, 100); // unloaded
        c(e.activeVoiceCount() == before, "an unloaded slot does not play");
        e.hit(3, 100);
        c(e.activeVoiceCount() == before + 1, "an empty-zone slot plays as a pad");
        c(!e.setSlotParams(16, plain()) && !e.setSlotParams(-1, plain()), "setSlotParams rejects slot 16 / -1");
        failedCases += report("sm_zone_maps_note_to_slots", c) ? 0 : 1;
    }

    // sm_gain_pan_velocity_level: gainDb, pan (balance), velocity sensitivity, engine level and
    // mute, all exact against the formulas; a buffer swap on a sounding slot ends its voices with a
    // declick (no step above the declick's) and the new buffer plays on the next hit.
    {
        Checks c;
        auto s = makeSample(48000, sr, [](int) { return 0.5f; });
        auto other = makeSample(48000, sr, [](int) { return -0.5f; });
        auto first = [&](const SamplerSlotParams& p, int velocity, float level, bool mute, float& L, float& R) {
            SamplerEngine e;
            prep(e, sr);
            e.setLevel(level);
            e.setMute(mute);
            e.setSlotBuffer(0, &s->view);
            e.setSlotParams(0, p);
            std::vector<float> l, r;
            render(e, 256, 128, l, r, [&](int done) { if (done == 0) e.hit(0, velocity); });
            L = l[200]; R = r[200];
        };
        float L = 0, R = 0;
        SamplerSlotParams p = plain();
        p.gainDb = -6.0f;
        first(p, 127, 1.0f, false, L, R);
        const float g6 = 0.5f * std::pow(10.0f, -6.0f / 20.0f);
        c(std::fabs(L - g6) < 1e-6f && std::fabs(R - g6) < 1e-6f, "gain -6 dB: " + std::to_string(L));
        p = plain();
        p.pan = -1.0f;
        first(p, 127, 1.0f, false, L, R);
        c(L == 0.5f && R == 0.0f, "pan -1: L " + std::to_string(L) + " R " + std::to_string(R));
        p.pan = 0.5f;
        first(p, 127, 1.0f, false, L, R);
        c(std::fabs(L - 0.25f) < 1e-6f && R == 0.5f, "pan 0.5: L " + std::to_string(L) + " R " + std::to_string(R));
        p = plain();
        first(p, 64, 1.0f, false, L, R);
        c(std::fabs(L - 0.5f * 64.0f / 127.0f) < 1e-6f, "velocity 64, sens 1: " + std::to_string(L));
        p.velocitySens = 0.0f;
        first(p, 1, 1.0f, false, L, R);
        c(L == 0.5f, "velocity 1, sens 0: " + std::to_string(L));
        p.velocitySens = 0.5f;
        first(p, 1, 1.0f, false, L, R);
        c(std::fabs(L - 0.5f * (0.5f + 0.5f / 127.0f)) < 1e-6f, "velocity 1, sens 0.5: " + std::to_string(L));
        first(plain(), 127, 0.5f, false, L, R);
        c(std::fabs(L - 0.25f) < 1e-6f, "level 0.5: " + std::to_string(L));
        first(plain(), 127, 1.0f, true, L, R);
        c(L == 0.0f && R == 0.0f, "mute: exact 0");
        // Attack: 1 ms rises linearly from 0.
        SamplerSlotParams a = plain();
        a.attackMs = 1.0f;
        {
            SamplerEngine e;
            prep(e, sr);
            e.setLevel(1.0f);
            e.setSlotBuffer(0, &s->view);
            e.setSlotParams(0, a);
            std::vector<float> l, r;
            render(e, 128, 128, l, r, [&](int) { e.hit(0, 127); });
            c(l[0] < 0.02f && std::fabs(l[24] - 0.25f) < 0.02f && l[60] == 0.5f, "1 ms attack: " + std::to_string(l[0]) +
              " / " + std::to_string(l[24]) + " / " + std::to_string(l[60]));
        }
        // Swap the buffer under a sounding voice.
        {
            SamplerEngine e;
            prep(e, sr);
            e.setLevel(1.0f);
            e.setSlotBuffer(0, &s->view);
            e.setSlotParams(0, plain());
            std::vector<float> l, r;
            render(e, 512, 128, l, r, [&](int done) {
                if (done == 0) e.hit(0, 127);
                if (done == 256) e.setSlotBuffer(0, &other->view);
            });
            c(e.activeVoiceCount() == 0, "swap ends the slot's voices");
            c(maxStep(l, 250, 512) <= 0.5f / (float)SamplerEngine::kDeclickSamples * 1.01f, "swap declicked: step " +
              std::to_string(maxStep(l, 250, 512)));
            std::vector<float> l2, r2;
            render(e, 128, 128, l2, r2, [&](int) { e.hit(0, 127); });
            c(l2[100] == -0.5f, "the new buffer plays on the next hit");
            e.setSlotBuffer(0, nullptr);
            c(e.activeVoiceCount() == 0, "unloading ends the slot's voices");
        }
        failedCases += report("sm_gain_pan_velocity_level", c) ? 0 : 1;
    }

    const uint64_t allocs = gRenderAllocs.load(), frees = gRenderFrees.load();
    const bool pass = failedCases == 0 && allocs == 0 && frees == 0;
    std::printf("sm    sampler voice core: hit, pitch, one-shot, release, loop, choke, steal, zones, gain  %-4s %d failed,"
                " render allocs %llu frees %llu\n", pass ? "PASS" : "FAIL", failedCases, (unsigned long long)allocs,
                (unsigned long long)frees);
    std::fflush(stdout);
    return pass;
}

// ── smb: render budget, 32 voices ────────────────────────────────────────────────────────────
// sm_budget_32v: 32 looped, pitched (non-integer step), panned stereo voices at 48 kHz in
// 128-frame blocks for 20 s. The spec's budget: at most 60 µs per block on a desktop core.
static bool runSamplerBudget() {
    using namespace smtest;
    const double sr = 48000.0;
    std::vector<std::unique_ptr<TestSample>> samples;
    SamplerEngine e;
    e.prepare(sr, 512);
    for (int slot = 0; slot < kSamplerSlots; ++slot) {
        samples.push_back(makeSample(96000, 44100.0, [slot](int i) { return noise(i, (uint32_t)slot + 7); },
                                     [slot](int i) { return noise(i, (uint32_t)slot + 70); }));
        e.setSlotBuffer(slot, &samples.back()->view);
        SamplerSlotParams p;
        p.oneShot = false;
        p.loop = true;
        p.loopStartMs = 200.0f;
        p.loopEndMs = 1500.0f;
        p.pan = (float)slot / 8.0f - 1.0f;
        p.tuneCents = 13.0f;
        p.lowNote = 0; p.highNote = 127;
        e.setSlotParams(slot, p);
    }
    for (int k = 0; k < 2; ++k) e.noteOn(50 + k * 7, 100); // 2 notes x 16 slots = 32 voices
    const int total = (int)(20.0 * sr);
    std::vector<float> l(128), r(128);
    int minActive = kSamplerVoices;
    double worstUs = 0.0;
    gRenderAllocs.store(0); gRenderFrees.store(0);
    const auto t0 = std::chrono::steady_clock::now();
    for (int done = 0; done < total; done += 128) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        const auto b0 = std::chrono::steady_clock::now();
        tCountAllocs = true;
        e.render(l.data(), r.data(), 128);
        tCountAllocs = false;
        worstUs = std::max(worstUs, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - b0).count());
        minActive = std::min(minActive, e.activeVoiceCount());
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double perBlockUs = secs * 1e6 / (double)(total / 128);
    const uint64_t allocs = gRenderAllocs.load();
    const bool pass = perBlockUs <= 60.0 && minActive == kSamplerVoices && allocs == 0 && gRenderFrees.load() == 0;
    std::printf("smb   sm_budget_32v: 32 looped pitched stereo voices, 48k/128, 20 s  %-4s %.2f us per block (limit 60),"
                " worst %.1f us, %.4f x realtime, min active %d, allocs %llu\n", pass ? "PASS" : "FAIL", perBlockUs, worstUs,
                secs / 20.0, minActive, (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}
