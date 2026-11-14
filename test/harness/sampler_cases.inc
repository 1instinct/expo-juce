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

// ── sml: loading and the hand-off (ios/SamplerBank.h / .mm) ──────────────────────────────────

static bool runSamplerLoader() {
    using namespace smtest;
    int failedCases = 0;
    std::printf("sml   sampler loader and hand-off (ios/SamplerBank.h)\n");
    auto codeOf = [](const std::string& e) { return e.substr(0, e.find(':')); };

    // sml_load_wav_and_mp3: a stereo 44.1 kHz WAV resampled to the wanted 48 kHz (a 1 kHz sine
    // stays a 1 kHz sine), a mono 48 kHz WAV kept as is on both channels, the MP3 fixture; a missing
    // file and a file that is not audio fail with their codes and publish nothing.
    {
        Checks c;
        SamplerBank bank;
        bank.requestRate(48000.0);
        juce::AudioBuffer<float> st(2, 22050);
        for (int i = 0; i < 22050; ++i) {
            st.setSample(0, i, 0.5f * (float)std::sin(2.0 * M_PI * 1000.0 * i / 44100.0));
            st.setSample(1, i, 0.25f * (float)std::cos(2.0 * M_PI * 1000.0 * i / 44100.0));
        }
        const std::string stPath = writeWav(st, 44100.0, "ej-sampler-st.wav", 2);
        SamplerLoadInfo info;
        std::string error;
        const bool okSt = bank.load(0, bank.beginLoad(0), stPath, info, error);
        c(okSt, "stereo WAV loads: " + error);
        c(info.frames == 24000 && info.sampleRate == 48000.0 && info.channels == 2 && std::fabs(info.durationMs - 500.0) < 1e-6
          && info.sourceRate == 44100.0, "stereo info: frames " + std::to_string(info.frames) + " rate " +
          std::to_string(info.sampleRate) + " ch " + std::to_string(info.channels) + " ms " + std::to_string(info.durationMs));
        const SamplerSlotInfo si = bank.slotInfo(0);
        c(si.loaded && si.frames == 24000 && si.channels == 2 && si.bytes == 24000LL * 2 * 4, "slotInfo(0)");
        const SampleBuffer* b = bank.acquireSlot(0);
        float worst = 0.0f;
        if (b != nullptr && b->frames == 24000) {
            for (int i = 2; i < 23990; ++i) {
                worst = std::max(worst, std::fabs(b->L[i] - 0.5f * (float)std::sin(2.0 * M_PI * 1000.0 * i / 48000.0)));
                worst = std::max(worst, std::fabs(b->R[i] - 0.25f * (float)std::cos(2.0 * M_PI * 1000.0 * i / 48000.0)));
            }
        }
        c(b != nullptr && b->rate == 48000.0 && worst < 2e-3f, "resampled sine within 2e-3 of the ideal: " + std::to_string(worst));

        juce::AudioBuffer<float> mono(1, 4800);
        for (int i = 0; i < 4800; ++i) mono.setSample(0, i, noise(i, 5));
        const std::string monoPath = writeWav(mono, 48000.0, "ej-sampler-mono.wav", 1);
        const bool okMono = bank.load(1, bank.beginLoad(1), monoPath, info, error);
        const SampleBuffer* m = bank.acquireSlot(1);
        bool same = m != nullptr && m->frames == 4800;
        for (int i = 0; same && i < 4800; ++i) same = m->L[i] == m->R[i] && std::fabs(m->L[i] - noise(i, 5)) < 1.0f / 4194304.0f;
        c(okMono && info.channels == 1 && info.frames == 4800 && same, "mono 48 kHz kept as is on both channels");

        const std::string mp3 = std::string(HARNESS_FIXTURES) + "/loop-124bpm-44k.mp3";
        const bool okMp3 = bank.load(2, bank.beginLoad(2), mp3, info, error);
        c(okMp3 && std::abs(info.frames - 749820) <= 1 && info.sampleRate == 48000.0 && info.sourceRate == 44100.0,
          "MP3 fixture: frames " + std::to_string(info.frames) + " " + error);
        const double mp3Ms = info.durationMs;
        const int64_t mp3Frames = info.frames;

        const bool missing = bank.load(3, bank.beginLoad(3), "/nonexistent/x.wav", info, error);
        c(!missing && codeOf(error) == "ERR_SAMPLER_FILE", "missing file: " + error);
        const juce::File junk = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ej-sampler-junk.wav");
        junk.replaceWithText("this is not audio");
        const bool garbage = bank.load(3, bank.beginLoad(3), junk.getFullPathName().toStdString(), info, error);
        c(!garbage && codeOf(error) == "ERR_SAMPLER_FORMAT", "not audio: " + error);
        c(!bank.slotInfo(3).loaded && bank.acquireSlot(3) == nullptr, "failed loads publish nothing");
        c(bank.residentBytes() == (24000LL + 4800 + mp3Frames) * 8, "resident bytes " + std::to_string(bank.residentBytes()));
        c(!bank.load(16, 0, stPath, info, error) && codeOf(error) == "ERR_SAMPLER_SLOT", "slot 16 rejected: " + error);
        junk.deleteFile();
        juce::File(juce::String(stPath)).deleteFile();
        juce::File(juce::String(monoPath)).deleteFile();
        char note[96];
        std::snprintf(note, sizeof(note), "resample error %.5f, MP3 %.0f ms", worst, mp3Ms);
        failedCases += report("sml_load_wav_and_mp3", c, note) ? 0 : 1;
        bank.releaseReader();
    }

    // sml_supersede_returns_SUPERSEDED: a newer beginLoad or an unload supersedes a load (before
    // it starts, between decode chunks, at install); the superseded load publishes nothing.
    {
        Checks c;
        SamplerBank bank;
        juce::AudioBuffer<float> a(1, 144000); // 3 s at 48 kHz: three decode chunks
        for (int i = 0; i < 144000; ++i) a.setSample(0, i, noise(i, 9));
        const std::string path = writeWav(a, 48000.0, "ej-sampler-3s.wav", 1);
        SamplerLoadInfo info;
        std::string error;
        const uint64_t g1 = bank.beginLoad(1);
        const uint64_t g2 = bank.beginLoad(1);
        c(!bank.load(1, g1, path, info, error) && codeOf(error) == "ERR_SAMPLER_SUPERSEDED", "older generation: " + error);
        c(!bank.slotInfo(1).loaded, "nothing published");
        int calls = 0;
        auto partial = SamplerBank::decode(path, 0.0, [&] { return ++calls >= 2; }, -1, info, error);
        c(partial == nullptr && codeOf(error) == "ERR_SAMPLER_SUPERSEDED" && calls == 2, "cancelled between chunks: " + error);
        const uint64_t g3 = bank.beginLoad(2);
        bank.unload(2);
        auto ready = SamplerBank::fromAudio(a.getReadPointer(0), a.getReadPointer(0), 1000, 48000.0);
        c(!bank.install(2, std::move(ready), g3, error) && codeOf(error) == "ERR_SAMPLER_SUPERSEDED", "unload supersedes: " + error);
        c(bank.load(1, g2, path, info, error), "the newest load still succeeds: " + error);
        bank.unloadAll();
        bank.collect();
        c(SamplerSample::live.load() == 0, "nothing left alive: " + std::to_string(SamplerSample::live.load()));
        juce::File(juce::String(path)).deleteFile();
        failedCases += report("sml_supersede_returns_SUPERSEDED", c) ? 0 : 1;
    }

    // sml_too_long: 61 s is refused (ERR_SAMPLER_TOO_LONG), 59 s loads.
    {
        Checks c;
        SamplerBank bank;
        bank.requestRate(48000.0);
        SamplerLoadInfo info;
        std::string error;
        juce::AudioBuffer<float> longAudio(1, 8000 * 61);
        longAudio.clear();
        const std::string longPath = writeWav(longAudio, 8000.0, "ej-sampler-61s.wav", 1);
        c(!bank.load(0, bank.beginLoad(0), longPath, info, error) && codeOf(error) == "ERR_SAMPLER_TOO_LONG", "61 s: " + error);
        const std::string tooLong = error;
        juce::AudioBuffer<float> okAudio(1, 8000 * 59);
        okAudio.clear();
        const std::string okPath = writeWav(okAudio, 8000.0, "ej-sampler-59s.wav", 1);
        c(bank.load(0, bank.beginLoad(0), okPath, info, error) && info.frames == 48000 * 59, "59 s loads: " + error);
        juce::File(juce::String(longPath)).deleteFile();
        juce::File(juce::String(okPath)).deleteFile();
        bank.unloadAll();
        bank.collect();
        failedCases += report("sml_too_long", c, tooLong) ? 0 : 1;
    }

    // sml_memory_cap: the total across slots is capped (the slot being replaced does not count);
    // a file larger than what is left fails before decoding; low available memory refuses too.
    {
        Checks c;
        SamplerBank bank;
        bank.setMemoryCapBytes(1024 * 1024);
        std::vector<float> x(60000, 0.1f);
        std::string error;
        auto make = [&] { return SamplerBank::fromAudio(x.data(), x.data(), 60000, 48000.0); }; // 480000 bytes
        c(bank.install(0, make(), bank.beginLoad(0), error), "slot 0: " + error);
        c(bank.install(1, make(), bank.beginLoad(1), error), "slot 1: " + error);
        c(!bank.install(2, make(), bank.beginLoad(2), error) && codeOf(error) == "ERR_SAMPLER_MEMORY", "slot 2 over the cap: " + error);
        const std::string capError = error;
        c(bank.install(0, make(), bank.beginLoad(0), error), "replacing slot 0 fits: " + error);
        c(bank.residentBytes() == 960000, "resident " + std::to_string(bank.residentBytes()));
        bank.unload(1);
        c(bank.install(2, make(), bank.beginLoad(2), error), "after unloading slot 1, slot 2 fits: " + error);
        juce::AudioBuffer<float> big(1, 200000); // 1.6 MB once stereo float
        big.clear();
        const std::string bigPath = writeWav(big, 48000.0, "ej-sampler-big.wav", 1);
        SamplerLoadInfo info;
        c(!bank.load(3, bank.beginLoad(3), bigPath, info, error) && codeOf(error) == "ERR_SAMPLER_MEMORY", "file over the cap: " + error);
        bank.setMemoryCapBytes(SamplerBank::kDefaultMemoryCapBytes);
        SamplerBank::setAvailableMemoryProvider([] { return (int64_t)1000; });
        c(!bank.load(3, bank.beginLoad(3), bigPath, info, error) && codeOf(error) == "ERR_SAMPLER_MEMORY", "low available memory: " + error);
        SamplerBank::setAvailableMemoryProvider(nullptr);
        c(bank.load(3, bank.beginLoad(3), bigPath, info, error), "loads under the default cap: " + error);
        juce::File(juce::String(bigPath)).deleteFile();
        bank.unloadAll();
        bank.collect();
        c(SamplerSample::live.load() == 0 && bank.residentBytes() == 0, "all freed");
        failedCases += report("sml_memory_cap", c, capError) ? 0 : 1;
    }

    // sml_handoff_keeps_buffer_in_use: the buffer the reader acquired survives collect() after it
    // is replaced, and is freed once the reader has moved to the new one; releaseReader lets go.
    {
        Checks c;
        SamplerBank bank;
        std::vector<float> x(1000, 0.1f);
        std::string error;
        bank.install(0, SamplerBank::fromAudio(x.data(), x.data(), 1000, 48000.0), bank.beginLoad(0), error);
        const SampleBuffer* first = bank.acquireSlot(0);
        bank.install(0, SamplerBank::fromAudio(x.data(), x.data(), 500, 48000.0), bank.beginLoad(0), error);
        c(bank.collect() == 1 && SamplerSample::live.load() == 2, "the acquired buffer is kept");
        c(first != nullptr && first->frames == 1000, "and still readable");
        const SampleBuffer* second = bank.acquireSlot(0);
        c(second != nullptr && second->frames == 500, "the reader moves to the new buffer");
        c(bank.collect() == 0 && SamplerSample::live.load() == 1, "then the old one is freed");
        bank.unload(0);
        c(bank.collect() == 1, "an unloaded buffer still acquired is kept");
        bank.releaseReader();
        c(bank.collect() == 0 && SamplerSample::live.load() == 0, "releaseReader lets it go");
        failedCases += report("sml_handoff_keeps_buffer_in_use", c) ? 0 : 1;
    }

    const bool pass = failedCases == 0;
    std::printf("sml   sampler loader: formats, supersede, too long, memory cap, hand-off  %-4s %d failed\n",
                pass ? "PASS" : "FAIL", failedCases);
    std::fflush(stdout);
    return pass;
}

// ── sms: loads / unloads while the audio thread plays ────────────────────────────────────────
// sml_swap_while_playing_no_alloc_on_audio_thread: a render thread runs beginBlock + hits + note-ons
// + render against the bank while the control thread installs and unloads 400 times and collects:
// 0 allocations and 0 frees on the audio thread, finite output, every buffer freed at the end.
static bool runSamplerSwapStress() {
    using namespace smtest;
    SamplerBank bank;
    bank.requestRate(48000.0);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> blocks{0};
    std::atomic<int> nonFinite{0};
    std::atomic<int> soundingBlocks{0};
    int pendingMax = 0, installed = 0;
    {
        SamplerEngine e;
        e.prepare(48000.0, 1024);
        e.setSource(&bank);
        for (int s = 0; s < kSamplerSlots; ++s) {
            SamplerSlotParams p;
            p.oneShot = (s % 2) == 0;
            p.loop = (s % 4) == 1;
            p.chokeGroup = s % 3;
            e.setSlotParams(s, p);
        }
        gRenderAllocs.store(0); gRenderFrees.store(0);
        std::thread audio([&] {
            std::vector<float> l(1024), r(1024);
            const int sizes[3] = {128, 471, 1024};
            for (uint64_t b = 0; !stop.load(); ++b) {
                const int n = sizes[b % 3];
                std::fill(l.begin(), l.end(), 0.0f);
                std::fill(r.begin(), r.end(), 0.0f);
                tCountAllocs = true;
                e.beginBlock();
                e.hit((int)(b % kSamplerSlots), 100);
                if (b % 5 == 0) e.noteOn(48 + (int)(b % 24), 90);
                if (b % 7 == 0) e.noteOff(48 + (int)((b + 12) % 24));
                e.render(l.data(), r.data(), n);
                tCountAllocs = false;
                bool sounding = false;
                for (int i = 0; i < n; ++i) {
                    if (!std::isfinite(l[(size_t)i]) || !std::isfinite(r[(size_t)i])) nonFinite.fetch_add(1);
                    sounding = sounding || l[(size_t)i] != 0.0f;
                }
                if (sounding) soundingBlocks.fetch_add(1);
                blocks.fetch_add(1);
            }
        });
        std::string error;
        for (int i = 0; i < 400; ++i) {
            const int slot = (i * 7) % kSamplerSlots;
            if (i % 9 == 8) {
                bank.unload(slot);
            } else {
                const int frames = 2000 + 53 * i;
                std::vector<float> x((size_t)frames);
                for (int k = 0; k < frames; ++k) x[(size_t)k] = 0.3f * noise(k, (uint32_t)i);
                installed += bank.install(slot, SamplerBank::fromAudio(x.data(), x.data(), frames, 48000.0), bank.beginLoad(slot), error) ? 1 : 0;
            }
            if (i % 3 == 0) std::this_thread::sleep_for(std::chrono::microseconds(300));
            pendingMax = std::max(pendingMax, bank.collect());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.store(true);
        audio.join();
    }
    bank.releaseReader(); // the reader is gone (what ~PolySynthProcessor does)
    const uint64_t allocs = gRenderAllocs.load(), frees = gRenderFrees.load();
    bank.unloadAll();
    const int pendingAfter = bank.collect();
    const int live = SamplerSample::live.load();
    const bool pass = allocs == 0 && frees == 0 && nonFinite.load() == 0 && pendingAfter == 0 && live == 0 && blocks.load() > 50
                   && pendingMax <= kSamplerSlots && installed == 400 - 400 / 9 && soundingBlocks.load() > blocks.load() / 2;
    std::printf("sms   sml_swap_while_playing_no_alloc_on_audio_thread: 400 install/unload while rendering (%llu blocks, %d"
                " sounding)  %-4s installed %d, render allocs %llu frees %llu, non-finite %d, max retired waiting %d, live after"
                " unload %d\n", (unsigned long long)blocks.load(), soundingBlocks.load(), pass ? "PASS" : "FAIL", installed,
                (unsigned long long)allocs, (unsigned long long)frees, nonFinite.load(), pendingMax, live);
    std::fflush(stdout);
    return pass;
}

// ── smq: through PolySynthProcessor and the note queue ───────────────────────────────────────
// Sampler events pushed from another thread sound in the next block (stereo: a hard-left pan
// leaves the right channel at 0), outside diag.peak and the note counters, never reach the arp;
// key note-offs, pad releases and allNotesOff (marks, so they cannot be dropped) end gated voices;
// out-of-range slots / notes are not queued; discardPending and a full ring count sampler events
// apart; the synth's prepare passes the device rate to the bank and its destruction releases the
// bank's reader; 0 allocations in processBlock.
static bool runSamplerInSynth() {
    using namespace smtest;
    const double sr = 48000.0;
    Checks c;
    gRenderAllocs.store(0); gRenderFrees.store(0);
    auto block = [&](PolySynthProcessor& synth, juce::AudioBuffer<float>& buf, int n) {
        juce::MidiBuffer midi;
        buf.setSize(2, n, false, false, true);
        tCountAllocs = true;
        synth.processBlock(buf, midi);
        tCountAllocs = false;
        return buf.getMagnitude(0, 0, n);
    };
    auto voicesOn = [](PolySynthProcessor& synth, int slot) {
        int n = 0;
        for (int v = 0; v < kSamplerVoices; ++v) {
            const SamplerVoiceInfo info = synth.sampler().voiceInfo(v);
            n += (info.active && info.slot == slot) ? 1 : 0;
        }
        return n;
    };
    for (int arpOn = 0; arpOn < 2; ++arpOn) {
        const std::string tag = arpOn ? " (arp on)" : " (arp off)";
        SamplerBank bank;
        std::vector<float> x(48000, 0.5f);
        std::string error;
        bank.install(5, SamplerBank::fromAudio(x.data(), x.data(), 48000, sr), bank.beginLoad(5), error);
        bank.install(6, SamplerBank::fromAudio(x.data(), x.data(), 48000, sr), bank.beginLoad(6), error);
        {
            TransportEngine transport;
            PolySynthProcessor synth;
            NoteEventQueue queue;
            synth.setNoteQueue(&queue);
            synth.setTransport(&transport);
            synth.setSamplerSource(&bank);
            synth.setRateAndBufferSizeDetails(sr, 512);
            synth.prepareToPlay(sr, 512);
            c(bank.wantedSampleRate() == sr, "prepare passes the rate to the bank" + tag);
            ArpSettings s = ArpSettings::defaults();
            s.enabled = arpOn == 1;
            synth.arp().setSettings(s);
            SamplerParamStore& store = synth.sampler().params();
            store.setLevel(1.0);
            SamplerSlotParams left;
            left.attackMs = 0.0f;
            left.pan = -1.0f;
            store.setSlot(5, left);
            SamplerSlotParams gated;
            gated.attackMs = 0.0f;
            gated.oneShot = false;
            gated.releaseMs = 1.0f;
            gated.lowNote = gated.highNote = 60;
            store.setSlot(6, gated);
            juce::AudioBuffer<float> buf(2, 512);
            c(block(synth, buf, 128) == 0.0f, "silent before" + tag);
            std::thread producer([&] { queue.pushSamplerHit(5, 127); });
            producer.join();
            const float p = block(synth, buf, 128);
            c(p == 0.5f && buf.getSample(0, 0) == 0.5f, "hit sounds from the block's first sample" + tag + " " + std::to_string(p));
            c(buf.getMagnitude(1, 0, 128) == 0.0f, "pan -1: right channel 0" + tag);
            c(synth.diag.peak.load() == 0.0f, "sampler not in the synth peak" + tag);
            c(synth.sampler().diag.peak.load() > 0.4f, "sampler peak diag" + tag);
            c(queue.samplerEventsQueued.load() == 1 && queue.samplerEventsConsumed.load() == 1, "sampler counters" + tag);
            c(queue.notesQueued.load() == 0 && queue.drumHitsQueued.load() == 0, "note / drum counters untouched" + tag);
            c(!synth.arp().isActive(), "the arp never gets sampler events" + tag);
            c(synth.diag.lastNoteToRenderMs.load() >= 0.0, "latency recorded" + tag);
            // Keys: note-on, then note-off through the marks.
            queue.pushSamplerNoteOn(60, 127);
            block(synth, buf, 128);
            c(voicesOn(synth, 6) == 1, "key note-on plays the zone's slot" + tag);
            queue.pushSamplerNoteOff(60);
            for (int i = 0; i < 3; ++i) block(synth, buf, 128);
            c(voicesOn(synth, 6) == 0, "key note-off releases it" + tag);
            // Pad on a gated slot: hit, then release(slot).
            synth.samplerHit(6, 100);
            block(synth, buf, 128);
            c(voicesOn(synth, 6) == 1, "gated pad hit" + tag);
            synth.samplerRelease(6);
            for (int i = 0; i < 3; ++i) block(synth, buf, 128);
            c(voicesOn(synth, 6) == 0, "pad release ends it" + tag);
            // allNotesOff releases gated sampler voices (keys and pads).
            synth.samplerNoteOn(60, 100);
            synth.samplerHit(6, 100);
            block(synth, buf, 128);
            c(voicesOn(synth, 6) == 2, "two gated voices" + tag);
            synth.allNotesOff();
            for (int i = 0; i < 3; ++i) block(synth, buf, 128);
            c(voicesOn(synth, 6) == 0, "allNotesOff releases them" + tag);
            // A release before the event's note-on is drained still applies to it (marks).
            synth.samplerNoteOn(60, 100);
            synth.samplerNoteOff(60);
            for (int i = 0; i < 3; ++i) block(synth, buf, 128);
            c(voicesOn(synth, 6) == 0, "a note-off queued right after its note-on ends it" + tag);
            const uint32_t queued = queue.samplerEventsQueued.load();
            c(!queue.pushSamplerHit(16, 100) && !queue.pushSamplerHit(-1, 100) && !queue.pushSamplerNoteOn(128, 100)
              && !queue.pushSamplerNoteOn(-1, 100) && queue.samplerEventsQueued.load() == queued, "out of range not queued" + tag);
        } // ~PolySynthProcessor releases the bank's reader
        bank.unloadAll();
        c(bank.collect() == 0 && SamplerSample::live.load() == 0, "the synth's destruction released the bank's reader" + tag);
    }
    {
        NoteEventQueue queue;
        queue.pushNoteOn(60, 100);
        queue.pushSamplerHit(1, 100);
        queue.pushSamplerNoteOn(61, 100);
        queue.pushDrumHit(2, 100);
        queue.discardPending();
        c(queue.notesDiscarded.load() == 1 && queue.samplerEventsDiscarded.load() == 2 && queue.drumHitsDiscarded.load() == 1,
          "discardPending counts by kind");
        int full = 0;
        for (int i = 0; i < NoteEventQueue::kSize + 5; ++i) full += queue.pushSamplerHit(0, 100) ? 0 : 1;
        c(full == 6 && queue.samplerEventsDropped.load() == 6, "a full ring drops sampler events (counted) " + std::to_string(full));
        c(queue.notesDropped.load() == 0 && queue.drumHitsDropped.load() == 0, "drops counted apart");
    }
    const uint64_t allocs = gRenderAllocs.load(), frees = gRenderFrees.load();
    c(allocs == 0 && frees == 0, "render allocations " + std::to_string(allocs) + "/" + std::to_string(frees));
    const bool pass = c.failed == 0;
    std::printf("smq   sampler through PolySynthProcessor + note queue (arp off / on): hits, keys, releases, allNotesOff, counters"
                "  %-4s %d failed checks allocs %llu%s\n", pass ? "PASS" : "FAIL", c.failed, (unsigned long long)allocs,
                c.detail.c_str());
    std::fflush(stdout);
    return pass;
}

// ── smd: src/samplerDefaults.ts == the C++ defaults ──────────────────────────────────────────
static bool runSamplerDefaultsMatchJs() {
    const juce::File file(juce::String(HARNESS_JS_SRC) + "/samplerDefaults.ts");
    const juce::String text = file.loadFileAsString();
    auto body = [&](const char* name) {
        const juce::String from = text.fromFirstOccurrenceOf(juce::String("export const ") + name, false, false);
        return from.fromFirstOccurrenceOf("({", false, false).upToFirstOccurrenceOf("})", false, false);
    };
    auto value = [](const juce::String& b, const char* key, bool& found) {
        juce::StringArray pairs;
        pairs.addTokens(b, ",", "");
        pairs.trim();
        for (const juce::String& p : pairs)
            if (p.upToFirstOccurrenceOf(":", false, false).trim() == key) {
                found = true;
                return p.fromFirstOccurrenceOf(":", false, false).trim();
            }
        found = false;
        return juce::String();
    };
    int mismatches = 0;
    std::string detail;
    auto num = [&](const juce::String& b, const char* key, double want) {
        bool found = false;
        const juce::String v = value(b, key, found);
        if (!found || std::fabs(v.getDoubleValue() - want) > 1e-6) { ++mismatches; detail += std::string(" ") + key; }
    };
    auto literal = [&](const juce::String& b, const char* key, const char* want) {
        bool found = false;
        if (value(b, key, found) != juce::String(want) || !found) { ++mismatches; detail += std::string(" ") + key; }
    };
    const SamplerSlotParams p;
    const juce::String slot = body("SAMPLER_SLOT_DEFAULTS");
    num(slot, "rootNote", p.rootNote);
    num(slot, "lowNote", p.lowNote);
    num(slot, "highNote", p.highNote);
    num(slot, "startMs", p.startMs);
    num(slot, "endMs", p.endMs);
    literal(slot, "loop", p.loop ? "?" : "null");
    literal(slot, "oneShot", p.oneShot ? "true" : "false");
    num(slot, "gainDb", p.gainDb);
    num(slot, "pan", p.pan);
    num(slot, "tune", p.tuneSemis);
    num(slot, "fineCents", p.tuneCents);
    num(slot, "attackMs", p.attackMs);
    num(slot, "releaseMs", p.releaseMs);
    num(slot, "velocitySens", p.velocitySens);
    num(slot, "chokeGroup", p.chokeGroup);
    const SamplerSettings st;
    const juce::String eng = body("SAMPLER_DEFAULTS");
    num(eng, "level", st.level);
    literal(eng, "mute", st.mute ? "true" : "false");
    num(eng, "polyphony", st.polyphony);
    auto constant = [&](const char* name, int want) {
        const juce::String v = text.fromFirstOccurrenceOf(juce::String("export const ") + name, false, false)
                                   .fromFirstOccurrenceOf("=", false, false).upToFirstOccurrenceOf(";", false, false).trim();
        if (v.isEmpty() || v.getIntValue() != want) { ++mismatches; detail += std::string(" ") + name + "(" + v.toStdString() + ")"; }
    };
    constant("SAMPLER_SLOT_COUNT", kSamplerSlots);
    constant("SAMPLER_VOICE_COUNT", kSamplerVoices);
    const bool pass = file.existsAsFile() && mismatches == 0;
    std::printf("smd   src/samplerDefaults.ts == SamplerEngine.h defaults, slot and voice counts  %-4s %d mismatches%s\n",
                pass ? "PASS" : "FAIL", mismatches, detail.c_str());
    std::fflush(stdout);
    return pass;
}
