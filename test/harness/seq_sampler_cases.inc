// Loop sequence sampler events (0.10.0-rc.2): case id `seqsm`. Included by harness.cpp after the
// sequence cases (SeqRig, seqNote, seqDrum, seqSampleOf) and sampler_cases.inc (smtest helpers).
// Sub-cases: seqsm_offsets, seqsm_release, seqsm_stop, seqsm_live, seqsm_mute, seqsm_smoothing,
// seqsm_rt, seqsm_validate, seqsm_only.

namespace seqsm {

using smtest::Checks;
using smtest::report;

static seqplay::Event hitEv(double start, double length, int slot, int velocity = 127) {
    seqplay::Event e;
    e.start = start; e.length = length; e.note = (uint8_t)slot; e.velocity = (uint8_t)velocity;
    e.kind = seqplay::EventKind::SamplerHit;
    return e;
}
static seqplay::Event keyEv(double start, double length, int note, int velocity = 127) {
    seqplay::Event e;
    e.start = start; e.length = length; e.note = (uint8_t)note; e.velocity = (uint8_t)velocity;
    e.kind = seqplay::EventKind::SamplerKey;
    return e;
}

struct Rec { int64_t at; int note; bool on; seqplay::EventKind kind; uint32_t tag; double beat; };

// The player alone, driven like PolySynthProcessor drives it, every output recorded (`out` must
// have its capacity reserved: push_back runs with the allocation counter on).
static void runPlayer(SeqRig& rig, int64_t total, const std::vector<int>& blocks, std::vector<Rec>& out, uint64_t& allocs) {
    size_t bi = 0;
    for (int64_t done = 0; done < total;) {
        const int n = (int)std::min<int64_t>(blocks[bi++ % blocks.size()], total - done);
        gRenderAllocs.store(0); gRenderFrees.store(0);
        tCountAllocs = true;
        rig.transport.advanceBlock(n, rig.sr);
        rig.player.process(rig.transport.getBlockStartBeat(), rig.transport.getBlockBpm(), rig.transport.isBlockPlaying(),
                           rig.transport.wasBlockSeeked(), rig.sr, n, [&](const seqplay::Output& o) {
                               out.push_back({rig.clock + o.offset, o.note, o.on, o.kind, o.tag, o.atBeat});
                           });
        tCountAllocs = false;
        allocs += gRenderAllocs.load() + gRenderFrees.load();
        rig.clock += n;
        done += n;
    }
}

// Samples for the synth rigs: 1 s of constant 0.5 in slot 5 (one-shot) and 6 (gated, 1 ms release,
// key zone 60 only), 2000 frames of 0.5 in slot 7 (one-shot). Slots 5 and 7 are off the keys.
struct Bank {
    std::vector<float> x = std::vector<float>(48000, 0.5f);
    SamplerBank bank;
    explicit Bank(double sr) {
        std::string error;
        bank.install(5, SamplerBank::fromAudio(x.data(), x.data(), 48000, sr), bank.beginLoad(5), error);
        bank.install(6, SamplerBank::fromAudio(x.data(), x.data(), 48000, sr), bank.beginLoad(6), error);
        bank.install(7, SamplerBank::fromAudio(x.data(), x.data(), 2000, sr), bank.beginLoad(7), error);
    }
    ~Bank() {
        bank.unloadAll();
        bank.collect();
    }
};

// PolySynthProcessor with a transport, a note queue, the bank, a sequence store and a note log;
// sampler level 1, attack 0. 120 bpm at 48 kHz: 24000 samples a beat. The whole stereo output is
// kept in L / R (capacity reserved, so recording never allocates under the counter).
struct Rig {
    double sr;
    TransportEngine transport;
    NoteEventQueue queue;
    PolySynthProcessor synth;
    std::unique_ptr<seqplay::SequenceStore> store = std::make_unique<seqplay::SequenceStore>();
    seqplay::NoteLog log;
    juce::AudioBuffer<float> buf { 2, 1024 };
    std::vector<float> L, R;
    uint64_t allocs = 0;

    Rig(SamplerBank& bank, double rate, bool arpOn = false) : sr(rate) {
        synth.setNoteQueue(&queue);
        synth.setTransport(&transport);
        synth.setSamplerSource(&bank);
        synth.setRateAndBufferSizeDetails(sr, 1024);
        synth.prepareToPlay(sr, 1024);
        synth.setSequenceStore(store.get());
        synth.setSequenceNoteLog(&log);
        ArpSettings a = ArpSettings::defaults();
        a.enabled = arpOn;
        synth.arp().setSettings(a);
        SamplerParamStore& p = synth.sampler().params();
        p.setLevel(1.0);
        SamplerSlotParams oneShot;
        oneShot.attackMs = 0.0f;
        oneShot.lowNote = 1;
        oneShot.highNote = 0; // pads only
        p.setSlot(5, oneShot);
        p.setSlot(7, oneShot);
        SamplerSlotParams gated;
        gated.attackMs = 0.0f;
        gated.oneShot = false;
        gated.releaseMs = 1.0f;
        gated.lowNote = gated.highNote = 60;
        p.setSlot(6, gated);
        L.reserve(1 << 20);
        R.reserve(1 << 20);
        log.listening.store(true);
    }

    // Renders `total` samples in `block`-sized blocks; returns the peak of what it rendered.
    float run(int64_t total, int block = 471) {
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int64_t done = 0; done < total;) {
            const int n = (int)std::min<int64_t>(block, total - done);
            buf.setSize(2, n, false, false, true);
            gRenderAllocs.store(0); gRenderFrees.store(0);
            tCountAllocs = true;
            synth.processBlock(buf, midi);
            tCountAllocs = false;
            allocs += gRenderAllocs.load() + gRenderFrees.load();
            L.insert(L.end(), buf.getReadPointer(0), buf.getReadPointer(0) + n);
            R.insert(R.end(), buf.getReadPointer(1), buf.getReadPointer(1) + n);
            peak = std::max(peak, std::max(buf.getMagnitude(0, 0, n), buf.getMagnitude(1, 0, n)));
            done += n;
        }
        return peak;
    }
    // Renders up to absolute sample `at`.
    float runTo(int64_t at, int block = 471) { return at > now() ? run(at - now(), block) : 0.0f; }
    int64_t now() const { return (int64_t)L.size(); }

    // Active sampler voices on `slot` (-1 = any); fromSeq 1 = the sequence's only, 0 = live only.
    int voices(int slot, int fromSeq = -1) const {
        int n = 0;
        for (int v = 0; v < kSamplerVoices; ++v) {
            const SamplerVoiceInfo info = synth.sampler().voiceInfo(v);
            if (!info.active || (slot >= 0 && info.slot != slot)) continue;
            if (fromSeq >= 0 && info.fromSequence != (fromSeq == 1)) continue;
            ++n;
        }
        return n;
    }
    // Largest |sample| on either channel in [from, to).
    float peak(int64_t from, int64_t to) const {
        float p = 0.0f;
        for (int64_t i = std::max<int64_t>(0, from); i < std::min<int64_t>(to, now()); ++i)
            p = std::max(p, std::max(std::fabs(L[(size_t)i]), std::fabs(R[(size_t)i])));
        return p;
    }
    float at(int64_t i) const { return i >= 0 && i < now() ? L[(size_t)i] : -1.0f; }
};

constexpr int64_t kBeat = 24000;   // samples per beat at 120 bpm, 48 kHz
constexpr int64_t kOneShotFade = 240; // kSamplerChokeMs at 48 kHz: sequenceStop's one-shot fade
constexpr int64_t kGatedRelease = 48; // the gated slot's 1 ms release

static std::string s(double v) { return std::to_string(v); }

// ── seqsm_offsets: exact sample offsets across block boundaries and on the loop wrap ──
static bool offsets(uint64_t& allocs) {
    Checks c;
    const std::vector<int> odd = {471, 128, 1024, 333, 64, 97};
    // The player: every sampler start on its sample, its release at start + length with the same
    // tag (no counting: two overlapping hits on slot 3 each get their own release), mixed with a
    // note and a drum hit, over odd blocks, three passes, events on and across the loop end.
    {
        SeqRig rig;
        const double Lb = 4.0, spb = 24000.0;
        std::vector<seqplay::Event> ev = { hitEv(0.0, 0.5, 3), hitEv(1.0 / 3.0, 0.1, 3), keyEv(1.0, 1.0, 60),
                                           hitEv(3.99, 0.5, 15), keyEv(4.0 - 1.0 / 24000.0, 0.25, 127),
                                           seqNote(2.0, 0.5, 64), seqDrum(2.5, 1) };
        const seqplay::SetResult r = rig.store->set(ev.data(), (int)ev.size(), Lb);
        c(r.ok && r.accepted == 7, "set");
        rig.transport.start();
        std::vector<Rec> out;
        out.reserve(8192);
        runPlayer(rig, (int64_t)(3 * Lb * spb), odd, out, allocs);
        struct W { int64_t at; int note; bool on; int kind; };
        std::vector<W> want, got;
        for (int pass = 0; pass < 3; ++pass) {
            for (const auto& e : ev) {
                const double t = pass * Lb + e.start;
                want.push_back({seqSampleOf(t, spb), e.note, true, (int)e.kind});
                if (seqplay::isHeldKind(e.kind) && t + e.length < 3 * Lb)
                    want.push_back({seqSampleOf(t + e.length, spb), e.note, false, (int)e.kind});
            }
        }
        for (const Rec& o : out) got.push_back({o.at, o.note, o.on, (int)o.kind});
        auto key = [](const W& w) { return std::make_tuple(w.at, w.on ? 1 : 0, w.note, w.kind); };
        std::sort(want.begin(), want.end(), [&](const W& a, const W& b) { return key(a) < key(b); });
        std::sort(got.begin(), got.end(), [&](const W& a, const W& b) { return key(a) < key(b); });
        int differ = 0;
        std::string first;
        for (size_t i = 0; i < std::min(want.size(), got.size()); ++i) {
            if (key(want[i]) != key(got[i]) && differ++ == 0)
                first = "want " + std::to_string(want[i].at) + (want[i].on ? " on " : " off ") + std::to_string(want[i].note) + " k"
                    + std::to_string(want[i].kind) + " got " + std::to_string(got[i].at) + (got[i].on ? " on " : " off ")
                    + std::to_string(got[i].note) + " k" + std::to_string(got[i].kind);
        }
        c(want.size() == got.size() && differ == 0, "player offsets: want " + std::to_string(want.size()) + " got "
          + std::to_string(got.size()) + ", " + std::to_string(differ) + " differ " + first);
        // Tags: unique per sampler start, its release carries it; notes and drums carry none.
        std::vector<uint32_t> onTags;
        int tagErrors = 0, stops = 0;
        for (const Rec& o : out) {
            if (o.kind == seqplay::EventKind::SamplerStop) { ++stops; continue; }
            if (!seqplay::isSamplerKind(o.kind)) { tagErrors += o.tag != 0 ? 1 : 0; continue; }
            if (o.on) {
                tagErrors += (o.tag == 0 || std::find(onTags.begin(), onTags.end(), o.tag) != onTags.end()) ? 1 : 0;
                onTags.push_back(o.tag);
                continue;
            }
            bool matched = false;
            for (const Rec& p : out)
                if (p.on && p.tag == o.tag && p.kind == o.kind && p.note == o.note && p.at < o.at) matched = true;
            tagErrors += matched ? 0 : 1;
        }
        c(tagErrors == 0, "tags: " + std::to_string(tagErrors) + " errors");
        c(stops == 0, "no SamplerStop while playing");
        c(rig.store->notesFired.load() == 3 * ev.size() && rig.store->lateFires.load() == 0, "fired / late");
        // Stop: the held sampler events are released by tag, then one SamplerStop, all at the
        // block start; nothing more in the next stopped blocks.
        const size_t before = out.size();
        const int64_t stopAt = rig.clock;
        rig.transport.stop();
        runPlayer(rig, 768, {256}, out, allocs);
        int stopOuts = 0, offsAtStop = 0, other = 0;
        for (size_t i = before; i < out.size(); ++i) {
            const Rec& o = out[i];
            if (o.kind == seqplay::EventKind::SamplerStop && o.at == stopAt && !o.on) ++stopOuts;
            else if (!o.on && o.at == stopAt) ++offsAtStop;
            else ++other;
        }
        c(stopOuts == 1 && other == 0, "stop: one SamplerStop at the block start (" + std::to_string(stopOuts) + ", other "
          + std::to_string(other) + ")");
        c(offsAtStop >= 1 && rig.player.activeNoteCount() == 0, "stop: held sampler events released");
    }
    // Through PolySynthProcessor (arp off and on): a 1-beat loop of slot 7 hits at 0.0 and 0.3 and a
    // key note 60 (gated slot 6) at 0.6 for 0.1 beat, 471-sample blocks, three passes: each starts
    // on its sample (the sample before is 0, its first is 0.5 on both channels), the key's release
    // starts on its sample and ends 1 ms later.
    for (int arpOn = 0; arpOn < 2; ++arpOn) {
        const std::string tag = arpOn ? " (arp on)" : " (arp off)";
        Bank bank(48000.0);
        Rig rig(bank.bank, 48000.0, arpOn == 1);
        std::vector<seqplay::Event> ev = { hitEv(0.0, 0.05, 7), hitEv(0.3, 0.1, 7), keyEv(0.6, 0.1, 60) };
        c(rig.store->set(ev.data(), 3, 1.0).ok, "synth set" + tag);
        rig.transport.start();
        rig.run(3 * kBeat, 471);
        for (int pass = 0; pass < 3; ++pass) {
            const int64_t base = pass * kBeat;
            for (int64_t k : { base, base + 7200, base + 14400 }) {
                const bool ok = rig.at(k) == 0.5f && rig.R[(size_t)k] == 0.5f && (k == 0 || rig.at(k - 1) == 0.0f);
                c(ok, "onset at " + std::to_string(k) + tag + ": " + s(rig.at(k - 1)) + " / " + s(rig.at(k)));
            }
            const int64_t off = base + 16800;
            c(rig.at(off - 1) == 0.5f && rig.at(off) == 0.5f && rig.at(off + 1) < 0.5f && rig.at(off + kGatedRelease - 1) > 0.0f
              && rig.peak(off + kGatedRelease, base + kBeat) == 0.0f,
              "key release at " + std::to_string(off) + tag + ": " + s(rig.at(off + 1)) + " / " + s(rig.at(off + kGatedRelease)));
        }
        c(rig.synth.diag.activeVoices.load() == 0 && !rig.synth.arp().isActive(), "no synth voice, the arp gets nothing" + tag);
        allocs += rig.allocs;
    }
    return report("seqsm_offsets", c);
}

// ── seqsm_release: release after the length ──
static bool release(uint64_t& allocs) {
    Checks c;
    Bank bank(48000.0);
    {
        // A gated hit for half a beat: full until sample 12000, its 1 ms release, then exact 0.
        Rig rig(bank.bank, 48000.0);
        std::vector<seqplay::Event> ev = { hitEv(0.0, 0.5, 6) };
        rig.store->set(ev.data(), 1, 4.0);
        rig.transport.start();
        rig.run(kBeat, 500);
        c(rig.at(11999) == 0.5f && rig.at(12000) == 0.5f && rig.at(12001) < 0.5f && rig.at(12047) > 0.0f
          && rig.peak(12048, kBeat) == 0.0f, "gated hit released after its length: " + s(rig.at(12001)) + " " + s(rig.at(12048)));
        c(rig.voices(6) == 0 && rig.store->activeNotes.load() == 0, "gated hit: no voice, nothing held");
        int ons = 0, offs = 0;
        rig.log.drain([&](const seqplay::NoteLog::Entry& e) {
            if (e.kind != seqplay::EventKind::SamplerHit || e.note != 6) return;
            if (e.on && e.atBeat == 0.0) ++ons;
            if (!e.on && e.atBeat == 0.5) ++offs;
        });
        c(ons == 1 && offs == 1, "note log: samplerHit on at 0, off at 0.5");
        allocs += rig.allocs;
    }
    {
        // A one-shot hit is not cut at its length: it plays on (the release is ignored).
        Rig rig(bank.bank, 48000.0);
        std::vector<seqplay::Event> ev = { hitEv(0.0, 0.25, 5) };
        rig.store->set(ev.data(), 1, 4.0);
        rig.transport.start();
        rig.run(kBeat / 2, 500);
        c(rig.peak(6000, 12000) == 0.5f && rig.at(11999) == 0.5f && rig.voices(5, 1) == 1, "one-shot plays past its length");
        allocs += rig.allocs;
    }
    {
        // A key note: on at 0.5, off at 1.0.
        Rig rig(bank.bank, 48000.0);
        std::vector<seqplay::Event> ev = { keyEv(0.5, 0.5, 60) };
        rig.store->set(ev.data(), 1, 4.0);
        rig.transport.start();
        rig.run(kBeat + 500, 500);
        c(rig.at(11999) == 0.0f && rig.at(12000) == 0.5f && rig.at(24000) == 0.5f && rig.peak(24048, kBeat + 500) == 0.0f,
          "key note on at its start, off at its end");
        int ons = 0, offs = 0;
        rig.log.drain([&](const seqplay::NoteLog::Entry& e) {
            if (e.kind != seqplay::EventKind::SamplerKey || e.note != 60) return;
            if (e.on && e.atBeat == 0.5) ++ons;
            if (!e.on && e.atBeat == 1.0) ++offs;
        });
        c(ons == 1 && offs == 1, "note log: samplerKey on / off");
        allocs += rig.allocs;
    }
    {
        // Two overlapping hits on one gated slot (0–2 and 1–3): each release ends its own voice.
        Rig rig(bank.bank, 48000.0);
        std::vector<seqplay::Event> ev = { hitEv(0.0, 2.0, 6), hitEv(1.0, 2.0, 6) };
        rig.store->set(ev.data(), 2, 4.0);
        rig.transport.start();
        rig.runTo(kBeat + kBeat / 2);
        c(rig.voices(6, 1) == 2, "two sequence voices on slot 6");
        rig.runTo(2 * kBeat + kBeat / 20);
        c(rig.voices(6, 1) == 1 && rig.at(rig.now() - 1) == 0.5f, "at 2.05 the first is released, the second sounds ("
          + std::to_string(rig.voices(6, 1)) + ")");
        rig.runTo(3 * kBeat + kBeat / 20);
        c(rig.voices(6) == 0 && rig.at(rig.now() - 1) == 0.0f, "at 3.05 both released");
        allocs += rig.allocs;
    }
    return report("seqsm_release", c);
}

// ── seqsm_stop: stop, jump, swap, loop length, clear, allNotesOff mid-hold ──
static bool stops(uint64_t& allocs) {
    Checks c;
    Bank bank(48000.0);
    const std::vector<seqplay::Event> ev = { hitEv(0.0, 3.0, 6), hitEv(0.0, 3.0, 5) };
    // A gated hold and a long one-shot from the sequence, at beat 1.
    auto holding = [&](Rig& rig) {
        rig.store->set(ev.data(), (int)ev.size(), 4.0);
        rig.transport.start();
        rig.runTo(kBeat);
        return rig.voices(6, 1) == 1 && rig.voices(5, 1) == 1 && rig.at(kBeat - 1) == 1.0f;
    };
    // Nothing the sequence started still sounds once the one-shot's 5 ms fade is over.
    auto silentAfter = [&](Rig& rig, int64_t t0, const std::string& what) {
        rig.runTo(t0 + 960, 128);
        c(rig.peak(t0 + kOneShotFade, t0 + 960) == 0.0f && rig.voices(-1) == 0 && rig.synth.sequence().activeNoteCount() == 0,
          what + ": nothing sounding (peak " + s(rig.peak(t0 + kOneShotFade, t0 + 960)) + ", voices "
              + std::to_string(rig.voices(-1)) + ")");
        c(rig.peak(t0, t0 + 1) > 0.0f, what + ": faded, not cut");
    };
    const std::pair<const char*, std::function<void(Rig&)>> actions[] = {
        { "stop", [](Rig& r) { r.transport.stop(); } },
        { "jump", [](Rig& r) { r.transport.setPosition(1.5); } },
        { "loop length", [&](Rig& r) { r.store->set(ev.data(), (int)ev.size(), 8.0); } },
        { "clear", [](Rig& r) { r.store->clear(); } },
        { "allNotesOff", [](Rig& r) { r.synth.allNotesOff(); } },
    };
    for (const auto& a : actions) {
        Rig rig(bank.bank, 48000.0);
        c(holding(rig), std::string(a.first) + ": holding");
        const int64_t t0 = rig.now();
        a.second(rig);
        silentAfter(rig, t0, a.first);
        if (std::string(a.first) == "jump") {
            rig.runTo(t0 + (int64_t)(2.6 * kBeat)); // from beat 1.5 to 4.1: the next pass plays
            c(rig.voices(6, 1) == 1 && rig.voices(5, 1) == 1, "jump: the next pass plays");
        }
        allocs += rig.allocs;
    }
    {
        // A swap that drops both events: the gated hold is released, the one-shot plays on (as a
        // drum hit does); a stop then still ends it.
        Rig rig(bank.bank, 48000.0);
        c(holding(rig), "swap: holding");
        const std::vector<seqplay::Event> other = { hitEv(2.5, 0.5, 7) };
        rig.store->set(other.data(), 1, 4.0);
        rig.runTo(kBeat + 600, 128);
        c(rig.voices(6) == 0 && rig.voices(5, 1) == 1, "swap: gated released, one-shot plays on");
        const int64_t t0 = rig.now();
        rig.transport.stop();
        silentAfter(rig, t0, "swap then stop");
        allocs += rig.allocs;
    }
    {
        // A swap that keeps the gated event with a new length (1.5): it sounds on and ends there.
        Rig rig(bank.bank, 48000.0);
        c(holding(rig), "swap keep: holding");
        const std::vector<seqplay::Event> kept = { hitEv(0.0, 1.5, 6), hitEv(0.0, 3.0, 5) };
        rig.store->set(kept.data(), 2, 4.0);
        rig.runTo(kBeat + kBeat * 4 / 10);
        c(rig.voices(6, 1) == 1, "swap keep: still sounding at 1.4");
        rig.runTo(kBeat + kBeat * 6 / 10);
        c(rig.voices(6) == 0 && rig.voices(5, 1) == 1, "swap keep: released at its new length 1.5");
        allocs += rig.allocs;
    }
    return report("seqsm_stop", c);
}

// ── seqsm_live: a live pad / key and the sequence on the same slot / note ──
static bool live(uint64_t& allocs) {
    Checks c;
    Bank bank(48000.0);
    for (int arpOn = 0; arpOn < 2; ++arpOn) {
        const std::string tag = arpOn ? " (arp on)" : " (arp off)";
        {
            // Pads, gated slot 6. Sequence: 0.25–0.5 and 1.0–3.0.
            Rig rig(bank.bank, 48000.0, arpOn == 1);
            const std::vector<seqplay::Event> ev = { hitEv(0.25, 0.25, 6), hitEv(1.0, 2.0, 6) };
            rig.store->set(ev.data(), 2, 4.0);
            rig.queue.pushSamplerHit(6, 127); // a live pad held from 0
            rig.transport.start();
            rig.runTo(kBeat * 3 / 8);
            c(rig.voices(6, 0) == 1 && rig.voices(6, 1) == 1 && rig.at(rig.now() - 1) == 1.0f, "live + sequence: two voices" + tag);
            rig.runTo(kBeat * 6 / 10);
            c(rig.voices(6, 0) == 1 && rig.voices(6, 1) == 0 && rig.at(rig.now() - 1) == 0.5f,
              "the sequence's release leaves the live hold" + tag);
            rig.queue.pushSamplerRelease(6);
            rig.runTo(kBeat * 7 / 10);
            c(rig.voices(6) == 0, "live release" + tag);
            rig.runTo(kBeat * 12 / 10); // the sequence holds 6 from 1.0
            rig.queue.pushSamplerHit(6, 127);
            rig.run(471);
            rig.queue.pushSamplerRelease(6); // releases every live holder of 6, not the sequence's
            rig.runTo(kBeat * 14 / 10);
            c(rig.voices(6, 1) == 1 && rig.voices(6, 0) == 0 && rig.at(rig.now() - 1) == 0.5f,
              "a live release leaves the sequence's hold" + tag);
            rig.queue.pushSamplerHit(6, 127);
            rig.runTo(kBeat * 15 / 10);
            c(rig.voices(6) == 2, "both held" + tag);
            rig.transport.stop();
            rig.run(960, 128);
            c(rig.voices(6, 1) == 0 && rig.voices(6, 0) == 1 && rig.at(rig.now() - 1) == 0.5f,
              "the sequence's stop leaves the live hold" + tag);
            rig.queue.pushSamplerRelease(6);
            rig.run(960, 128);
            c(rig.voices(-1) == 0, "all released" + tag);
            allocs += rig.allocs;
        }
        {
            // Keys, note 60 (slot 6's zone): the same both ways.
            Rig rig(bank.bank, 48000.0, arpOn == 1);
            const std::vector<seqplay::Event> ev = { keyEv(0.25, 0.25, 60), keyEv(1.0, 2.0, 60) };
            rig.store->set(ev.data(), 2, 4.0);
            rig.queue.pushSamplerNoteOn(60, 127);
            rig.transport.start();
            rig.runTo(kBeat * 6 / 10);
            c(rig.voices(6, 0) == 1 && rig.voices(6, 1) == 0, "the sequence's key off leaves the live key" + tag);
            rig.queue.pushSamplerNoteOff(60);
            rig.runTo(kBeat * 12 / 10);
            rig.queue.pushSamplerNoteOn(60, 127);
            rig.run(471);
            rig.queue.pushSamplerNoteOff(60);
            rig.runTo(kBeat * 14 / 10);
            c(rig.voices(6, 1) == 1 && rig.voices(6, 0) == 0, "a live key off leaves the sequence's key" + tag);
            rig.transport.stop();
            rig.run(960, 128);
            c(rig.voices(-1) == 0, "stop ends the sequence's key" + tag);
            allocs += rig.allocs;
        }
        {
            // A live one-shot keeps playing through the sequence's stop, which ends the sequence's one-shot.
            Rig rig(bank.bank, 48000.0, arpOn == 1);
            const std::vector<seqplay::Event> ev = { hitEv(0.0, 0.1, 5) };
            rig.store->set(ev.data(), 1, 4.0);
            rig.queue.pushSamplerHit(5, 127);
            rig.transport.start();
            rig.runTo(kBeat / 2);
            c(rig.voices(5, 0) == 1 && rig.voices(5, 1) == 1, "two one-shots" + tag);
            rig.transport.stop();
            rig.run(960, 128);
            c(rig.voices(5, 0) == 1 && rig.voices(5, 1) == 0 && rig.at(rig.now() - 1) == 0.5f,
              "stop ends the sequence's one-shot, not the live one" + tag);
            allocs += rig.allocs;
        }
    }
    return report("seqsm_live", c);
}

// ── seqsm_mute ──
static bool mute(uint64_t& allocs) {
    Checks c;
    Bank bank(48000.0);
    Rig rig(bank.bank, 48000.0);
    const std::vector<seqplay::Event> ev = { hitEv(0.0, 2.0, 6), hitEv(0.0, 0.1, 5) };
    rig.store->set(ev.data(), 2, 4.0);
    rig.queue.pushSamplerHit(5, 127); // live
    rig.transport.start();
    rig.runTo(kBeat / 2);
    c(rig.voices(6, 1) == 1 && rig.voices(5, 1) == 1 && rig.voices(5, 0) == 1, "holding");
    const int64_t t0 = rig.now();
    rig.store->setMuted(true);
    rig.runTo(t0 + 960, 128);
    c(rig.synth.sampler().sequenceVoiceCount() == 0 && rig.voices(5, 0) == 1, "mute ends the sequence's voices, not the live one");
    c(rig.peak(t0 + kOneShotFade, t0 + 960) == 0.5f, "only the live one-shot sounds");
    const uint64_t fired = rig.store->notesFired.load();
    rig.runTo(kBeat * 45 / 10);
    c(rig.synth.sampler().sequenceVoiceCount() == 0 && rig.store->notesFired.load() == fired, "nothing starts while muted");
    rig.store->setMuted(false);
    rig.runTo(kBeat * 82 / 10);
    c(rig.voices(6, 1) == 1 && rig.voices(5, 1) == 1, "unmuted: the next pass plays");
    allocs += rig.allocs;
    return report("seqsm_mute", c);
}

// ── seqsm_smoothing: with #11's parameter smoothing ──
// A cutoff step 200 → 8000 Hz while the sequence plays synth notes (A, B) with sampler events (A,
// C): the synth's smoothed cutoff is the same with and without the sampler events after every
// block, and A − B equals C (the sampler events add exactly the sampler's output, from the samples
// they start on, while the synth's segments and ramps run). The sampler's own level ramp gives the
// same output whether a block is rendered whole or in pieces.
static bool smoothing(uint64_t& allocs) {
    Checks c;
    Bank bankA(48000.0), bankB(48000.0), bankC(48000.0);
    Rig a(bankA.bank, 48000.0), b(bankB.bank, 48000.0), cc(bankC.bank, 48000.0);
    std::vector<seqplay::Event> notes, sampler;
    for (int k = 0; k < 32; ++k) notes.push_back(seqNote(k * 0.125, 0.0625, 48 + (k % 5) * 3));
    // 0.15 → 0.154 (samples 3600 → 3696): a start and its release in one 471-sample block.
    sampler = { hitEv(0.01, 0.05, 6), hitEv(0.07, 0.2, 5), keyEv(0.13, 0.05, 60), hitEv(0.15, 0.004, 6), hitEv(0.2, 0.05, 7) };
    std::vector<seqplay::Event> both = notes;
    both.insert(both.end(), sampler.begin(), sampler.end());
    a.store->set(both.data(), (int)both.size(), 4.0);
    b.store->set(notes.data(), (int)notes.size(), 4.0);
    cc.store->set(sampler.data(), (int)sampler.size(), 4.0);
    for (Rig* r : { &a, &b, &cc }) {
        r->synth.setWaveform(Waveform::Saw);
        r->synth.setMasterLevel(0.5);
        r->synth.setFilterCutoff(200.0);
        r->transport.start();
    }
    const int L = (int)std::floor(SynthParamSmoother::kRampSeconds * 48000.0);
    int cutoffDiffers = 0;
    for (int phase = 0; phase < 2; ++phase) {
        if (phase == 1) { a.synth.setFilterCutoff(8000.0); b.synth.setFilterCutoff(8000.0); cc.synth.setFilterCutoff(8000.0); }
        const int total = phase == 0 ? 4710 : L + 2 * 471;
        for (int done = 0; done < total; done += 471) {
            a.run(471); b.run(471); cc.run(471);
            if (a.synth.smoothedParams().currentCutoff() != b.synth.smoothedParams().currentCutoff()) ++cutoffDiffers;
        }
    }
    c(cutoffDiffers == 0 && a.synth.smoothedParams().currentCutoff() == 8000.0f,
      "smoothed cutoff unchanged by sampler events (" + std::to_string(cutoffDiffers) + " blocks differ)");
    float worst = 0.0f, samplerPeak = 0.0f;
    for (int64_t i = 0; i < a.now(); ++i) {
        worst = std::max(worst, std::fabs(a.L[(size_t)i] - b.L[(size_t)i] - cc.L[(size_t)i]));
        worst = std::max(worst, std::fabs(a.R[(size_t)i] - b.R[(size_t)i] - cc.R[(size_t)i]));
        samplerPeak = std::max(samplerPeak, std::fabs(cc.L[(size_t)i]));
    }
    c(worst < 1e-5f && samplerPeak > 0.4f, "A - B == C: worst " + s(worst));
    c(cc.at(240) == 0.5f && cc.at(239) == 0.0f, "C: the first hit on its sample (0.01 beat = 240)");
    // Slot 5's one-shot and key 60 (slot 6) sound at 1.0; the short slot-6 hit adds 0.5 on exactly
    // 3600 → 3695 and fades after its release, all inside the block at 3297.
    c(cc.at(3599) == 1.0f && cc.at(3600) == 1.5f && cc.at(3695) == 1.5f && std::fabs(cc.at(3800) - 1.0f) < 1e-6f
          && cc.at(3297) == 1.0f,
      "C: a start and its release in one block land on their samples (" + s(cc.at(3297)) + " " + s(cc.at(3599)) + " "
          + s(cc.at(3600)) + " " + s(cc.at(3695)) + " " + s(cc.at(3800)) + ")");
    allocs += a.allocs + b.allocs + cc.allocs;
    // The sampler's level ramp does not depend on how a block is split: a level step rendered as
    // one 471-sample block, and as pieces of 3, 61, 1 and 406, give the same output.
    {
        auto x = smtest::makeSample(48000, 48000.0, [](int i) { return smtest::noise(i); });
        float whole[471] = {}, wholeR[471] = {}, split[471] = {}, splitR[471] = {};
        for (int pass = 0; pass < 2; ++pass) {
            SamplerEngine e;
            smtest::prep(e, 48000.0);
            e.setSlotBuffer(0, &x->view);
            e.setSlotParams(0, smtest::plain());
            e.hit(0, 127);
            float tmpL[471], tmpR[471];
            e.render(tmpL, tmpR, 471); // sounding at level 1
            e.setLevel(0.25f);
            float* l = pass == 0 ? whole : split;
            float* r = pass == 0 ? wholeR : splitR;
            if (pass == 0) {
                e.render(l, r, 471);
            } else {
                int at = 0;
                for (int n : { 3, 61, 1, 406 }) { e.render(l + at, r + at, n); at += n; }
            }
        }
        float diff = 0.0f;
        for (int i = 0; i < 471; ++i) diff = std::max(diff, std::max(std::fabs(whole[i] - split[i]), std::fabs(wholeR[i] - splitR[i])));
        c(diff == 0.0f, "level ramp: split == whole (diff " + s(diff) + ")");
    }
    return report("seqsm_smoothing", c, "worst " + s(worst));
}

// ── seqsm_rt: #21's interruption cut, silence and discard with sequence voices ──
static bool realtime(uint64_t& allocs) {
    Checks c;
    for (int arpOn = 0; arpOn < 2; ++arpOn) {
        const std::string tag = arpOn ? " (arp on)" : " (arp off)";
        Bank bank(48000.0);
        const std::vector<seqplay::Event> ev = { hitEv(0.0, 3.0, 6), hitEv(0.0, 3.0, 5) };
        {
            // The interruption, in the engine's order with no block in between (the device has
            // stopped): stop the transport, allNotesOff, the cut, a full ring of live events sent
            // meanwhile, then requestDiscard. The first block back is exactly 0 and stays so,
            // nothing counts as busy; a restart plays the loop from its first sample.
            Rig rig(bank.bank, 48000.0, arpOn == 1);
            rig.store->set(ev.data(), 2, 4.0);
            rig.transport.start();
            rig.runTo(kBeat, 128);
            c(rig.voices(-1, 1) == 2, "holding" + tag);
            rig.transport.stop();
            rig.synth.allNotesOff();
            rig.queue.pushSamplerCut();
            for (int i = 0; i < NoteEventQueue::kSize; ++i) rig.queue.pushSamplerHit(5, 127);
            rig.queue.requestDiscard();
            const float back = rig.run(32, 32);
            float rest = rig.run(32 * 8, 32);
            c(back == 0.0f && rest == 0.0f, "interruption: the first block back is exactly 0 and stays 0" + tag + " " + s(back));
            bgpolicy::Activity act;
            act.voicesSounding = rig.synth.diag.activeVoices.load() + rig.synth.drums().diag.activeVoices.load()
                + rig.synth.sampler().diag.activeVoices.load();
            act.sequenceNotes = rig.store->activeNotes.load();
            c(rig.voices(-1) == 0 && !bgpolicy::isBusy(act), "interruption: no voice, not busy" + tag);
            rig.transport.setPosition(0.0);
            rig.transport.start();
            const int64_t t = rig.now();
            rig.run(128, 128);
            c(rig.at(t) == 1.0f, "restart: the loop plays from its first sample" + tag + " " + s(rig.at(t)));
            allocs += rig.allocs;
        }
        {
            // The cut with the transport still playing: the sequence's voices stop at once too;
            // the next pass plays.
            Rig rig(bank.bank, 48000.0, arpOn == 1);
            rig.store->set(ev.data(), 2, 4.0);
            rig.transport.start();
            rig.runTo(kBeat, 128);
            rig.queue.pushSamplerCut();
            const float back = rig.run(32, 32);
            c(back == 0.0f && rig.voices(-1) == 0, "cut covers the sequence's voices" + tag + " " + s(back));
            rig.runTo(4 * kBeat + 128, 128);
            c(rig.at(4 * kBeat) == 1.0f, "the next pass plays after the cut" + tag);
            allocs += rig.allocs;
        }
        {
            // The silence mark (the keepAlive-off background stop) with the transport playing:
            // the sequence's voices fade within 5 ms; the next pass plays.
            Rig rig(bank.bank, 48000.0, arpOn == 1);
            rig.store->set(ev.data(), 2, 4.0);
            rig.transport.start();
            rig.runTo(kBeat, 128);
            const int64_t t0 = rig.now();
            rig.queue.pushSamplerSilence();
            rig.run(512, 128);
            c(rig.at(t0) > 0.0f && rig.peak(t0 + kOneShotFade, t0 + 512) == 0.0f && rig.voices(-1) == 0,
              "silence fades the sequence's voices" + tag);
            rig.runTo(4 * kBeat + 128, 128);
            c(rig.at(4 * kBeat) == 1.0f, "the next pass plays after the silence" + tag);
            // keepAlive off in the background: stop + allNotesOff + silence.
            rig.runTo(5 * kBeat, 128);
            const int64_t t1 = rig.now();
            rig.transport.stop();
            rig.synth.allNotesOff();
            rig.queue.pushSamplerSilence();
            rig.run(512, 128);
            c(rig.peak(t1 + kOneShotFade, t1 + 512) == 0.0f && rig.voices(-1) == 0
              && rig.store->activeNotes.load() == 0, "background stop: nothing left" + tag);
            allocs += rig.allocs;
        }
    }
    return report("seqsm_rt", c);
}

// ── seqsm_validate: what refuses a whole sequence, what only skips an event ──
static bool validate() {
    Checks c;
    auto store = std::make_unique<seqplay::SequenceStore>();
    const std::vector<seqplay::Event> good = { hitEv(0.0, 1.0, 3), keyEv(1.0, 1.0, 60) };
    c(store->set(good.data(), 2, 4.0).ok, "good set");
    const uint64_t v1 = store->version();
    auto refused = [&](std::vector<seqplay::Event> ev, int index, seqplay::Reject why, const std::string& what) {
        const seqplay::SetResult r = store->set(ev.data(), (int)ev.size(), 4.0);
        c(!r.ok && r.rejectedIndex == index && r.reason == why, what + ": refused at " + std::to_string(r.rejectedIndex));
        c(store->version() == v1 && store->eventCount() == 2, what + ": the previous sequence stays");
        c(store->lastRejectedEvent() == index && store->lastReject() == why, what + ": diagnostics");
    };
    refused({ hitEv(0.0, 1.0, 3), keyEv(1.0, 1.0, 60), hitEv(2.0, 1.0, 16) }, 2, seqplay::Reject::SamplerSlot, "slot 16");
    refused({ keyEv(0.0, 1.0, 255) }, 0, seqplay::Reject::SamplerKeyNote, "key 255");
    {
        std::vector<seqplay::Event> ev = { seqNote(0.0, 1.0, 60), seqNote(1.0, 1.0, 61) };
        ev[1].kind = (seqplay::EventKind)9;
        refused(ev, 1, seqplay::Reject::Kind, "kind 9");
        ev[1].kind = seqplay::EventKind::SamplerStop;
        refused(ev, 1, seqplay::Reject::Kind, "SamplerStop in a sequence");
    }
    // Skipped, not refused (as notes always were): a length <= 0, a start outside the loop, a
    // non-finite length, and a synth note out of range; velocity clamped.
    const std::vector<seqplay::Event> mixed = { hitEv(0.0, 0.0, 3), hitEv(5.0, 1.0, 3), keyEv(0.0, NAN, 60),
                                                seqNote(0.0, 1.0, 200), hitEv(0.5, 1.0, 15, 0), keyEv(2.0, 0.5, 0) };
    const seqplay::SetResult r = store->set(mixed.data(), (int)mixed.size(), 4.0);
    c(r.ok && r.accepted == 2 && r.dropped == 4 && r.rejectedIndex == -1, "skipped: accepted " + std::to_string(r.accepted)
      + " dropped " + std::to_string(r.dropped));
    c(store->lastRejectedEvent() == -1 && store->lastReject() == seqplay::Reject::None, "a published set clears the rejection");
    const seqplay::Sequence* seq = store->acquire();
    c(seq && seq->count == 2 && seq->events[0].kind == seqplay::EventKind::SamplerHit && seq->events[0].velocity == 1
      && seq->events[1].kind == seqplay::EventKind::SamplerKey, "kept and clamped");
    store->release();
    return report("seqsm_validate", c);
}

// ── seqsm_only: a loop with sampler events only ──
static bool onlySampler(uint64_t& allocs) {
    Checks c;
    Bank bank(48000.0);
    Rig rig(bank.bank, 48000.0);
    const std::vector<seqplay::Event> ev = { hitEv(0.0, 0.5, 6), keyEv(0.5, 0.25, 60) };
    rig.store->set(ev.data(), 2, 1.0);
    rig.transport.start();
    rig.runTo(kBeat / 4);
    bgpolicy::Activity act; // the transport left out: the holds alone keep it busy
    act.voicesSounding = rig.synth.sampler().diag.activeVoices.load();
    act.sequenceNotes = rig.store->activeNotes.load();
    c(rig.store->firing.load() && rig.store->activeNotes.load() == 1 && act.voicesSounding == 1 && bgpolicy::isBusy(act),
      "playing, holding, busy");
    rig.runTo(2 * kBeat + kBeat / 4);
    rig.transport.stop();
    rig.run(960, 128);
    c(!rig.store->firing.load() && rig.store->activeNotes.load() == 0 && rig.voices(-1) == 0, "stopped: nothing held");
    struct E { int kind; int note; bool on; double beat; };
    std::vector<E> got;
    rig.log.drain([&](const seqplay::NoteLog::Entry& e) { got.push_back({(int)e.kind, e.note, e.on, e.atBeat}); });
    const int H = (int)seqplay::EventKind::SamplerHit, K = (int)seqplay::EventKind::SamplerKey;
    std::vector<E> want;
    for (int pass = 0; pass < 3; ++pass) {
        want.push_back({H, 6, true, pass + 0.0});
        if (pass < 2) {
            want.push_back({H, 6, false, pass + 0.5});
            want.push_back({K, 60, true, pass + 0.5});
            want.push_back({K, 60, false, pass + 0.75});
        }
    }
    bool same = got.size() == want.size() + 1; // + the hit of pass 2 released by the stop
    for (size_t i = 0; same && i < want.size(); ++i)
        same = got[i].kind == want[i].kind && got[i].note == want[i].note && got[i].on == want[i].on && got[i].beat == want[i].beat;
    same = same && got.back().kind == H && !got.back().on; // its release on the stop; no SamplerStop entry
    c(same, "note log: kinds, notes, on / off and beats (" + std::to_string(got.size()) + " entries)");
    allocs += rig.allocs;
    return report("seqsm_only", c);
}

} // namespace seqsm

static bool runSequenceSampler() {
    std::printf("seqsm sequence sampler events (0.10.0-rc.2): offsets, release, stop, live, mute, smoothing, rt, validate, sampler only\n");
    std::fflush(stdout);
    uint64_t allocs = 0;
    int failed = 0;
    failed += seqsm::offsets(allocs) ? 0 : 1;
    failed += seqsm::release(allocs) ? 0 : 1;
    failed += seqsm::stops(allocs) ? 0 : 1;
    failed += seqsm::live(allocs) ? 0 : 1;
    failed += seqsm::mute(allocs) ? 0 : 1;
    failed += seqsm::smoothing(allocs) ? 0 : 1;
    failed += seqsm::realtime(allocs) ? 0 : 1;
    failed += seqsm::validate() ? 0 : 1;
    failed += seqsm::onlySampler(allocs) ? 0 : 1;
    const bool pass = failed == 0 && allocs == 0;
    std::printf("seqsm sequence sampler events  %-4s %d failed, render allocs %llu\n", pass ? "PASS" : "FAIL", failed,
                (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}
