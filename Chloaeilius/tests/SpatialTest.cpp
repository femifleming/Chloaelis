// Offline checks for Chloaeilius's sphere (not shipped): where the dry sounds and the echoes land,
// that every channel is AmbiX, that the echoes fly apart, and how much CPU it takes.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/PluginProcessor.h"
#include <chrono>
#include <cstdio>

using namespace juce;
using fp::ambi::Vec3;

static int failures = 0;
static void check (bool ok, const char* what, const String& detail = {})
{
    std::printf ("  [%s] %s %s\n", ok ? " ok " : "FAIL", what, detail.toRawUTF8());
    if (! ok) ++failures;
}
static double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }
static constexpr double SR = 48000.0;
static constexpr int BLOCK = 256;

static void setEnv (const char* k, const char* v)
{
   #if JUCE_WINDOWS
    _putenv_s (k, v);
   #else
    setenv (k, v, 1);
   #endif
}

static void set (ChloaelisProcessor& p, const String& id, float real)
{
    if (auto* prm = p.apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (real));
}

struct Run { std::vector<std::vector<float>> out; double cpu = 0; bool finite = true; };

template <typename Gen>
static Run render (ChloaelisProcessor& p, double seconds, Gen gen, bool keep = true)
{
    AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (AudioChannelSet::stereo());
    layout.outputBuses.add (AudioChannelSet::discreteChannels (16));
    p.setBusesLayout (layout);
    p.setRateAndBufferSizeDetails (SR, BLOCK);
    p.prepareToPlay (SR, BLOCK);
    AudioBuffer<float> buf (16, BLOCK);
    MidiBuffer midi;
    Run r;
    if (keep) r.out.assign (16, {});
    const int total = (int) (seconds * SR);
    const auto t0 = std::chrono::steady_clock::now();
    for (int pos = 0; pos < total; pos += BLOCK)
    {
        buf.clear();
        for (int i = 0; i < BLOCK; ++i) { auto v = gen (pos + i); buf.setSample (0, i, v.first); buf.setSample (1, i, v.second); }
        p.processBlock (buf, midi);
        for (int c = 0; c < 16; ++c)
            for (int i = 0; i < BLOCK; ++i)
            {
                const float x = buf.getSample (c, i);
                if (! std::isfinite (x)) r.finite = false;
                if (keep) r.out[(size_t) c].push_back (x);
            }
    }
    r.cpu = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / seconds;
    return r;
}

static void writeWav (const File& f, const std::vector<std::vector<float>>& ch)
{
    f.deleteFile();
    WavAudioFormat wav;
    if (auto w = std::unique_ptr<AudioFormatWriter> (wav.createWriterFor (new FileOutputStream (f), SR, (unsigned int) ch.size(), 24, {}, 0)))
    {
        AudioBuffer<float> b ((int) ch.size(), (int) ch[0].size());
        for (int c = 0; c < (int) ch.size(); ++c) b.copyFrom (c, 0, ch[(size_t) c].data(), (int) ch[0].size());
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    setEnv ("CHL_NO_LASTSTATE", "1");
    const File outDir = argc > 1 ? File (argv[1]) : File::getCurrentWorkingDirectory();
    Random rnd (7);
    auto noiseLeft = [&] (int) { return std::pair<float, float> { (rnd.nextFloat() * 2.0f - 1.0f) * 0.2f, 0.0f }; };
    auto noiseBoth = [&] (int) { const float v = (rnd.nextFloat() * 2.0f - 1.0f) * 0.2f; return std::pair<float, float> { v, (rnd.nextFloat() * 2.0f - 1.0f) * 0.2f }; };
    // short clicks of noise a few times a second: echoes you can follow
    auto bursts = [&] (int i)
    {
        const int k = i % (int) (SR * 0.7);
        const float e = k < (int) (0.03 * SR) ? 1.0f - (float) k / (float) (0.03 * SR) : 0.0f;
        const float v = e * (rnd.nextFloat() * 2.0f - 1.0f) * 0.5f;
        return std::pair<float, float> { v, v };
    };
    auto ratio = [] (ChloaelisProcessor& p, Vec3 d) { return 10.0 * std::log10 ((p.energy.levelAt (d.normalised()) + 1e-12) / (p.energy.levelAt (d.normalised() * -1.0f) + 1e-12)); };

    std::printf ("the dry sound\n");
    {
        ChloaelisProcessor p;
        for (int s = 0; s < 2; ++s) set (p, chl::pid (s, chl::wet), 0.0f);
        auto r = render (p, 2.0, noiseLeft);
        const auto leftFront = fp::ambi::fromAzEl (MathConstants<float>::pi / 3.0f, 0.0f);
        check (r.finite, "16 channels out, all finite");
        check (ratio (p, leftFront) > 10.0, "the left input plays from the front left", String (ratio (p, leftFront), 1) + " dB over the opposite side");

        // every channel is its harmonic (the direct path alone, the walls switched off)
        fp::DistanceSource::withWalls = false;
        r = render (p, 1.0, noiseLeft);
        fp::DistanceSource::withWalls = true;
        const auto y = fp::ambi::sh (leftFront);
        double w2 = 0, worst = 0;
        for (size_t i = (size_t) SR / 2; i < r.out[0].size(); ++i) w2 += (double) r.out[0][i] * r.out[0][i];
        for (int c = 0; c < 16; ++c)
        {
            double cw = 0;
            for (size_t i = (size_t) SR / 2; i < r.out[0].size(); ++i) cw += (double) r.out[(size_t) c][i] * r.out[0][i];
            worst = std::max (worst, std::abs (cw / w2 - y[(size_t) c]));
        }
        check (worst < 0.01, "each channel is its harmonic (ACN / SN3D, as the IEM decoders expect)", "worst error " + String (worst, 5));
    }

    std::printf ("the echoes\n");
    {
        ChloaelisProcessor p;
        set (p, chl::widthId, 1.0f);   // all seven poly taps
        set (p, chl::flockId, 1.0f);
        set (p, chl::lzSpeedId, 0.6f);
        for (int s = 0; s < 2; ++s) { set (p, chl::pid (s, chl::wet), 1.0f); set (p, chl::pid (s, chl::regen), 0.6f); }
        auto r = render (p, 10.0, bursts, false);
        // where the 18 points are: spread out, not on top of each other
        float minD = 9.0f, maxR = 0.0f;
        std::vector<Vec3> pts;
        for (int s = 0; s < 2; ++s)
            for (int j = 0; j < ChloaelisProcessor::kPoints; ++j)
                pts.push_back ({ p.points[(size_t) s][(size_t) j].x.load(), p.points[(size_t) s][(size_t) j].y.load(), p.points[(size_t) s][(size_t) j].z.load() });
        for (size_t a = 0; a < pts.size(); ++a)
        {
            maxR = std::max (maxR, pts[a].length());
            for (size_t b = a + 1; b < pts.size(); ++b) minD = std::min (minD, (pts[a] - pts[b]).length());
        }
        check (r.finite && maxR <= 1.0001f, "18 points (2 dry, 2 main echoes, 14 taps), all inside the sphere", "closest pair " + String (minD, 2) + ", furthest out " + String (maxR, 2));
        double lo = 1e9, hi = -1e9;
        for (auto d : { Vec3 { 1, 0, 0 }, Vec3 { -1, 0, 0 }, Vec3 { 0, 1, 0 }, Vec3 { 0, -1, 0 } })
        { const double l = 10.0 * std::log10 (p.energy.levelAt (d) + 1e-12); lo = std::min (lo, l); hi = std::max (hi, l); }
        check (hi - lo < 15.0, "the echoes come from all round (front, back, left, right)", String (hi - lo, 1) + " dB between the loudest and quietest side");
        check (r.cpu < 0.6, "fast enough", String (r.cpu * 100.0, 1) + " % of one core in real time");

        // the points move: where they are 2 s later is somewhere else
        auto before = pts;
        render (p, 2.0, bursts, false);
        float moved = 0;
        for (int s = 0; s < 2; ++s)
            for (int j = 1; j < ChloaelisProcessor::kPoints; ++j)
            {
                const Vec3 q { p.points[(size_t) s][(size_t) j].x.load(), p.points[(size_t) s][(size_t) j].y.load(), p.points[(size_t) s][(size_t) j].z.load() };
                moved = std::max (moved, (q - before[(size_t) (s * ChloaelisProcessor::kPoints + j)]).length());
            }
        check (moved > 0.05f, "the echoes fly (the flock and the attractor move them)", "furthest move in 2 s " + String (moved, 2));
    }

    std::printf ("gain staging (stereo noise in at -18 dBFS per channel, uncorrelated; W out)\n");
    {
        Random gr (9);
        const float amp = 0.125f * std::sqrt (3.0f);
        auto noise18 = [&] (int) { return std::pair<float, float> { (gr.nextFloat() * 2.0f - 1.0f) * amp, (gr.nextFloat() * 2.0f - 1.0f) * amp }; };
        auto rmsFrom = [] (const std::vector<float>& x, size_t from) { double s2 = 0; for (size_t i = from; i < x.size(); ++i) s2 += (double) x[i] * x[i]; return std::sqrt (s2 / (double) std::max<size_t> (1, x.size() - from)); };
        const double inDb = db (amp / std::sqrt (3.0));
        double dryDb = 0;
        auto measure = [&] (const char* what, std::function<void (ChloaelisProcessor&)> setup, double lo, double hi, bool relToDry)
        {
            ChloaelisProcessor p;
            setup (p);
            auto r = render (p, 5.0, noise18);
            const double w = db (rmsFrom (r.out[0], (size_t) (2 * SR)));
            float pk = 0; for (size_t i = (size_t) (2 * SR); i < r.out[0].size(); ++i) pk = std::max (pk, std::abs (r.out[0][i]));
            const double rel = w - (relToDry ? dryDb : inDb);
            std::printf ("    %-40s W %6.1f dBFS  (%+5.1f dB vs %s)  peak %6.1f dBFS\n", what, w, rel, relToDry ? "dry" : "input", db (pk));
            check (r.finite && rel >= lo && rel <= hi, (String ("  levels: ") + what).toRawUTF8());
            return w;
        };
        auto wet = [] (ChloaelisProcessor& p, float v) { for (int s = 0; s < 2; ++s) set (p, chl::pid (s, chl::wet), v); };
        measure ("dry, tube off (the spatial side alone)", [&] (ChloaelisProcessor& p) { wet (p, 0.0f); for (int s = 0; s < 2; ++s) set (p, chl::tubeOnId (s), 0.0f); }, -2.0, 2.0, false);
        // (with the tube on, as Chloaelis starts, its 12AX7 at the default drive adds about 3 dB, as it does in Chloaelis)
        dryDb = measure ("dry (echoes off, tube on)", [&] (ChloaelisProcessor& p) { wet (p, 0.0f); }, -2.0, 4.0, false);
        measure ("defaults (wet 35 %, poly off)", [] (ChloaelisProcessor&) {}, -3.0, 3.0, true);
        measure ("echoes alone (wet 100 %)", [&] (ChloaelisProcessor& p) { wet (p, 1.0f); }, -6.0, 3.0, true);
        measure ("poly all the way (8 taps a side), wet 60 %", [&] (ChloaelisProcessor& p) { wet (p, 0.6f); set (p, chl::widthId, 1.0f); }, -4.0, 3.0, true);
        measure ("poly + flock, wet 100 %", [&] (ChloaelisProcessor& p) { wet (p, 1.0f); set (p, chl::widthId, 1.0f); set (p, chl::flockId, 1.0f); }, -6.0, 3.0, true);
        measure ("small bubble (1.5 m)", [&] (ChloaelisProcessor& p) { set (p, chl::spaceId, 0.0f); }, 2.0, 12.0, true);
        measure ("big bubble (24 m)", [&] (ChloaelisProcessor& p) { set (p, chl::spaceId, 1.0f); }, -18.0, -3.0, true);
        {
            ChloaelisProcessor p;
            auto r = render (p, 3.0, noise18);
            check (p.levelerGainNow() > 0.999f && p.limiterGainNow() > 0.999f, "  levels: at -18 dBFS the leveler and limiter stay out of it");
            for (int s = 0; s < 2; ++s) set (p, chl::pid (s, chl::inAtt), 12.0f);
            set (p, chl::spaceId, 0.0f);
            r = render (p, 3.0, noise18);
            float pk = 0; for (auto& ch : r.out) for (auto x : ch) pk = std::max (pk, std::abs (x));
            check (pk <= 0.892f, "  levels: +12 dB in, smallest bubble: still under -1 dBFS", "peak " + String (db (pk), 2) + " dBFS");
        }
    }

    std::printf ("demo render (16 channels, for an IEM decoder) in %s\n", outDir.getFullPathName().toRawUTF8());
    {
        ChloaelisProcessor p;
        set (p, chl::widthId, 0.8f);
        set (p, chl::flockId, 0.7f);
        set (p, chl::srcLevelId, 0.9f);   // the built-in recording
        for (int s = 0; s < 2; ++s) { set (p, chl::pid (s, chl::wet), 0.6f); set (p, chl::pid (s, chl::regen), 0.55f); }
        auto r = render (p, 25.0, [] (int) { return std::pair<float, float> { 0.0f, 0.0f }; });
        writeWav (outDir.getChildFile ("chloaeilius_demo_ambix16.wav"), r.out);
        check (r.finite, "wrote chloaeilius_demo_ambix16.wav");
    }

    std::printf (failures == 0 ? "\nall passed\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
