// Runs the real plugin processor + editor headlessly: odd block sizes, sample rates,
// LFO modulation + freeze, state round trip, default-amount check, and a UI screenshot.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "ChlAudio.h"

using namespace juce;
extern int chloaelisDbgRenders, chloaelisDbgBlurs;
extern int chloaelisDbgFrames;

// portable environment helpers (the tests flip a flag that the processor reads)
static void setEnv (const char* k, const char* v)
{
   #if JUCE_WINDOWS
    _putenv_s (k, v);
   #else
    setenv (k, v, 1);
   #endif
}
static void unsetEnv (const char* k)
{
   #if JUCE_WINDOWS
    _putenv_s (k, "");
   #else
    unsetenv (k);
   #endif
}


// MANUAL_DUMP=<folder>: every parameter as JSON, for the manual
static int dumpForManual (const char* dir)
{
    const File out (dir);
    out.createDirectory();
    ChloaelisProcessor p;
    Array<var> params;
    for (auto* ap : p.getParameters())
        if (auto* r = dynamic_cast<RangedAudioParameter*> (ap))
        {
            DynamicObject::Ptr o = new DynamicObject();
            o->setProperty ("id", r->getParameterID());
            o->setProperty ("name", r->getName (100));
            o->setProperty ("default", r->getText (r->getDefaultValue(), 100));
            o->setProperty ("min", r->getText (0.0f, 100));
            o->setProperty ("max", r->getText (1.0f, 100));
            String type = "continuous";
            if (auto* c = dynamic_cast<AudioParameterChoice*> (r)) { type = "choice"; o->setProperty ("choices", c->choices.joinIntoString (", ")); }
            else if (dynamic_cast<AudioParameterBool*> (r) != nullptr) type = "switch";
            else if (dynamic_cast<AudioParameterInt*> (r) != nullptr) type = "steps";
            o->setProperty ("type", type);
            params.add (var (o.get()));
        }
    out.getChildFile ("params.json").replaceWithText (JSON::toString (var (params)));
    std::printf ("manual material: %d parameters in %s\n", params.size(), out.getFullPathName().toRawUTF8());
    return 0;
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    if (auto* md = std::getenv ("MANUAL_DUMP")) return dumpForManual (md);
    setEnv ("CHL_NO_LASTSTATE", "1");   // tests start from defaults and never touch the user's saved state
    int fails = 0;
    // SPACE_SHOTS=<folder>: each way of showing the sphere, with the sound running through it
    if (auto* dir = std::getenv ("SPACE_SHOTS"))
    {
        const File out (dir);
        out.createDirectory();
        ChloaelisProcessor p;
        p.setPlayConfigDetails (2, 16, 48000, 512);
        p.prepareToPlay (48000, 512);
        auto set = [&] (const String& id, float real) { if (auto* prm = p.apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (real)); };
        set (chl::widthId, 1.0f); set (chl::flockId, 0.8f); set (chl::lzSpeedId, 0.55f); set (chl::srcLevelId, 0.8f);
        for (int s = 0; s < 2; ++s) { set (chl::pid (s, chl::wet), 0.6f); set (chl::pid (s, chl::regen), 0.6f); }
        std::unique_ptr<AudioProcessorEditor> ed (p.createEditorAndMakeActive());
        ed->setSize (ChloaelisEditor::designW, ChloaelisEditor::designH);
        auto* ce = dynamic_cast<ChloaelisEditor*> (ed.get());
        AudioBuffer<float> buf (16, 512); MidiBuffer m;
        Random rnd (5);
        long t = 0;
        auto play = [&] (int blocks)
        {
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                for (int i = 0; i < 512; ++i, ++t)
                {
                    const int k = (int) (t % 30000);
                    const float e = k < 1500 ? 1.0f - k / 1500.0f : 0.0f;
                    const float v = e * (rnd.nextFloat() * 2.0f - 1.0f) * 0.4f;
                    buf.setSample (0, i, v); buf.setSample (1, i, v);
                }
                p.processBlock (buf, m);
                if (ce != nullptr) ce->advanceFrames (1);
            }
        };
        play (600);
        static const char* names[] = { "lorenz", "orrery", "map", "radar" };
        for (int v = 0; v < 4; ++v)
        {
            set (chl::viewId, (float) v);
            play (200);
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            auto f = out.getChildFile (String ("chloaeilius_view_") + names[v] + ".png");
            f.deleteFile();
            FileOutputStream os (f);
            PNGImageFormat().writeImageToStream (img, os);
            std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
        }
        // the big sphere, each way, and turned by hand
        set (chl::viewId, 1.0f);
        const std::tuple<int, float, float, const char*> big[] = { { 1, 0.0f, 0.0f, "globe" }, { 1, 1.9f, 0.7f, "globe_turned" }, { 2, 0.0f, 0.0f, "map" }, { 3, 0.0f, 0.0f, "radar" } };
        for (auto& [mode, yawB, pitchB, name] : big)
        {
            if (ce != nullptr) ce->showSphereForTest (mode, yawB, pitchB);
            play (200);
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            auto f = out.getChildFile (String ("chloaeilius_sphere_") + name + ".png");
            f.deleteFile();
            FileOutputStream os (f);
            PNGImageFormat().writeImageToStream (img, os);
            std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
        }
        if (ce != nullptr) ce->showSphereForTest (0);
        {
            // the desktop app's layout: the vessel and the listen switch (a fresh instance, as the app starts)
            ChloaelisProcessor::forceDesktop = true;
            ChloaelisProcessor pd;
            pd.setPlayConfigDetails (2, 16, 48000, 512);
            pd.prepareToPlay (48000, 512);
            pd.apvts.getParameter (chl::widthId)->setValueNotifyingHost (1.0f);
            std::unique_ptr<AudioProcessorEditor> de (pd.createEditorAndMakeActive());
            if (de != nullptr)
            {
                de->setSize (ChloaelisEditor::designW, ChloaelisEditor::designH);
                auto* dce = dynamic_cast<ChloaelisEditor*> (de.get());
                AudioBuffer<float> b2 (16, 512);
                for (int k = 0; k < 300; ++k) { b2.clear(); pd.processBlock (b2, m); if (dce != nullptr) dce->advanceFrames (1); }
                auto img = de->createComponentSnapshot (de->getLocalBounds(), true, 1.0f);
                auto f = out.getChildFile ("chloaeilius_desktop.png");
                f.deleteFile();
                FileOutputStream os (f);
                PNGImageFormat().writeImageToStream (img, os);
                std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
            }
            de.reset();
            ChloaelisProcessor::forceDesktop = false;
            ce = nullptr;
        }
        return 0;
    }
    // live frame rate: a real window on the real (OpenGL) renderer, audio running, the focus
    // band resting and then sweeping. Opens a window for a few seconds, so only on request.
    if (std::getenv ("CHL_AUDIOTEST") != nullptr)
    {
        // 1) the loops: stream straight through each seam and look for gaps and clicks
        auto makeReader = [] (const char* data, int size)
        {
            OggVorbisAudioFormat fmt;
            return std::unique_ptr<AudioFormatReader> (fmt.createReaderFor (new MemoryInputStream (data, (size_t) size, false), true));
        };
        struct Case { const char* name; const char* data; int size; double a, b, first, seamAt; float trim; };
        for (auto c : { Case { "base loop", ChlAudio::baseloop_ogg, ChlAudio::baseloop_oggSize, 6.0, 235.5, 220.0, 235.5, 0.0f },
                        Case { "hydrophone", ChlAudio::hydrophone3_ogg, ChlAudio::hydrophone3_oggSize, 4.0, -1.0, 120.0, 0.0, 3.5f } })
        {
            chl::LoopStream st;
            auto r = makeReader (c.data, c.size);
            const double len = (double) (r->lengthInSamples - 4096) / r->sampleRate;
            st.setup (std::move (r), c.a, c.b, 4.0, c.first, c.trim);
            st.prepare (48000.0);
            const double seam = c.seamAt > 0 ? c.seamAt : len;
            const int total = (int) ((seam - c.first + 6.0) * 48000.0);
            std::vector<float> L ((size_t) total), R ((size_t) total);
            int got = 0, under = 0;
            while (got < total)
            {
                st.useTimeSlice();
                const int n = jmin (512, total - got);
                if (! st.pullAdd (L.data() + got, R.data() + got, n, 1.0f, 1.0f)) ++under;
                got += n;
            }
            const int seamS = (int) ((seam - c.first) * 48000.0);
            auto maxJump = [&] (int a, int b) { float m = 0; for (int i = a + 1; i < b; ++i) m = jmax (m, std::abs (L[(size_t) i] - L[(size_t) i - 1])); return m; };
            auto rms = [&] (int a, int b) { double e = 0; for (int i = a; i < b; ++i) e += L[(size_t) i] * L[(size_t) i]; return 20.0 * std::log10 (std::sqrt (e / jmax (1, b - a)) + 1e-9); };
            std::printf ("%s: seam at %.1fs; underruns %d; rms before %.1f dB, across %.1f dB, after %.1f dB; max jump at seam %.3f vs. before %.3f\n",
                         c.name, seam, under, rms (seamS - 192000, seamS - 96000), rms (seamS - 2400, seamS + 2400), rms (seamS + 96000, seamS + 192000),
                         maxJump (seamS - 4800, seamS + 4800), maxJump (seamS - 96000, seamS - 4800));
        }
        // 2) poly all the way up: 8 taps per side, all three engines, finite, and what it costs
        for (int ty = 0; ty < 3; ++ty)
        {
            ChloaelisProcessor p;
            p.prepareToPlay (48000, 512);
            p.apvts.getParameter (chl::widthId)->setValueNotifyingHost (1.0f);
            p.apvts.getParameter (chl::flockId)->setValueNotifyingHost (1.0f);
            p.apvts.getParameter (chl::delayTypeId (0))->setValueNotifyingHost ((float) ty / 2.0f);
            p.apvts.getParameter (chl::delayTypeId (1))->setValueNotifyingHost ((float) ty / 2.0f);
            AudioBuffer<float> b (2, 512); MidiBuffer m; bool fin = true; float peak = 0; Random rnd (5);
            const double t0 = Time::getMillisecondCounterHiRes();
            const int blocks = 940;   // 10 s of audio
            for (int blk = 0; blk < blocks; ++blk)
            {
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) b.setSample (c, i, blk % 40 < 2 ? rnd.nextFloat() * 0.4f - 0.2f : 0.0f);
                p.processBlock (b, m);
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) { fin = fin && std::isfinite (b.getSample (c, i)); peak = jmax (peak, std::abs (b.getSample (c, i))); }
            }
            const double ms = Time::getMillisecondCounterHiRes() - t0;
            std::printf ("poly + flock 100%% %s: peak %.2f, finite %s, %.1f%% of real time\n", ty == 0 ? "tape" : ty == 1 ? "PT2399" : "BBD", peak, fin ? "yes" : "NO", ms / 100.0);
        }
        // 2b) master feedback crosses over: feed only the left input, the right output answers
        {
            ChloaelisProcessor p;
            p.prepareToPlay (48000, 512);
            p.apvts.getParameter (chl::masterFbId)->setValueNotifyingHost (0.7f);
            p.apvts.getParameter (chl::pid (0, chl::wet))->setValueNotifyingHost (0.0f);
            p.apvts.getParameter (chl::pid (1, chl::wet))->setValueNotifyingHost (0.0f);
            AudioBuffer<float> b (2, 512); MidiBuffer m; double eR = 0;
            for (int blk = 0; blk < 40; ++blk)
            {
                for (int i = 0; i < 512; ++i) { b.setSample (0, i, 0.3f * std::sin ((float) (blk * 512 + i) * 0.05f)); b.setSample (1, i, 0.0f); }
                p.processBlock (b, m);
                if (blk > 10) for (int i = 0; i < 512; ++i) eR += b.getSample (1, i) * b.getSample (1, i);
            }
            std::printf ("cross feedback: left in only -> right out %.1f dB (silent without the cross)\n", 10.0 * std::log10 (eR / (29 * 512) + 1e-12));
        }
        // 2c) last state: closing saves it, a new instance picks it up (the user's real file is
        //     backed up and put back)
        {
            auto base = File::getSpecialLocation (File::userApplicationDataDirectory);
           #if JUCE_MAC
            base = base.getChildFile ("Application Support");
           #endif
            const auto f = base.getChildFile ("Chloaelis/LastState.xml");
            const auto bak = f.getSiblingFile ("LastState.testbackup");
            const bool had = f.existsAsFile();
            if (had) f.copyFileTo (bak);
            unsetEnv ("CHL_NO_LASTSTATE");
            { ChloaelisProcessor a; a.apvts.getParameter (chl::pid (0, chl::regen))->setValueNotifyingHost (0.77f); }
            float got;
            { ChloaelisProcessor b; got = b.apvts.getParameter (chl::pid (0, chl::regen))->getValue(); }
            setEnv ("CHL_NO_LASTSTATE", "1");
            if (had) { bak.copyFileTo (f); bak.deleteFile(); } else f.deleteFile();
            std::printf ("last state: saved 0.77, new instance has %.2f %s\n", got, std::abs (got - 0.77f) < 0.01f ? "(ok)" : "(FAIL)");
        }
        // 2e) seamless switching: order, delay engine, time mode and tube type flipped while a
        //     tone runs through a ringing delay; look for clicks and dips around each switch
        {
            ChloaelisProcessor p;
            p.prepareToPlay (48000, 256);
            p.apvts.getParameter (chl::pid (0, chl::regen))->setValueNotifyingHost (0.55f);
            p.apvts.getParameter (chl::pid (0, chl::drive))->setValueNotifyingHost (0.5f);
            if (std::getenv ("CHL_SWITCH_DRY") != nullptr) p.apvts.getParameter (chl::pid (0, chl::wet))->setValueNotifyingHost (0.0f);
            AudioBuffer<float> b (2, 256); MidiBuffer m;
            struct Sw { const char* what; std::function<void()> act; };
            std::vector<Sw> sws {
                { "order", [&] { p.apvts.getParameter (chl::orderId)->setValueNotifyingHost (1.0f); } },
                { "delay engine", [&] { p.apvts.getParameter (chl::delayTypeId (0))->setValueNotifyingHost (0.0f); } },
                { "time mode", [&] { p.apvts.getParameter (chl::timeModeId (0))->setValueNotifyingHost (1.0f); } },
                { "tube type", [&] { p.apvts.getParameter (chl::tubeTypeId (0))->setValueNotifyingHost (1.0f / 3.0f); } } };
            std::vector<float> out;
            int at[4];
            for (int blk = 0; blk < 1000; ++blk)
            {
                if (blk % 200 == 150) { const int k = blk / 200; if (k < 4) { at[k] = (int) out.size(); sws[(size_t) k].act(); } }
                for (int i = 0; i < 256; ++i) { const float x = 0.25f * std::sin ((float) (blk * 256 + i) * 2.0f * 3.14159f * 330.0f / 48000.0f); b.setSample (0, i, x); b.setSample (1, i, x); }
                p.processBlock (b, m);
                for (int i = 0; i < 256; ++i) out.push_back (b.getSample (0, i));
            }
            auto rms = [&] (int a0, int a1) { double e = 0; for (int i = a0; i < a1; ++i) e += out[(size_t) i] * out[(size_t) i]; return 20.0 * std::log10 (std::sqrt (e / jmax (1, a1 - a0)) + 1e-9); };
            auto jump = [&] (int a0, int a1) { float mx = 0; for (int i = a0 + 1; i < a1; ++i) mx = jmax (mx, std::abs (out[(size_t) i] - out[(size_t) i - 1])); return mx; };
            for (int k = 0; k < 4; ++k)
            {
                const int c = at[k];
                std::printf ("switch %-12s  level before %.1f dB, lowest 10 ms after %.1f dB, largest step %.3f (before: %.3f)\n", sws[(size_t) k].what,
                             rms (c - 9600, c), [&] { double lo = 99; for (int w = 0; w < 9600; w += 480) lo = jmin (lo, rms (c + w, c + w + 480)); return lo; }(),
                             jump (c, c + 9600), jump (c - 9600, c));
                std::printf ("                settled, 0.4-0.6 s later: largest step %.3f\n", jump (c + 19200, c + 28800));
                { int bi = c; float mx = 0; for (int i = c + 1; i < c + 9600; ++i) { const float d = std::abs (out[(size_t) i] - out[(size_t) i - 1]); if (d > mx) { mx = d; bi = i; } }
                  std::printf ("                biggest step at +%.1f ms; steps > 0.08 after the switch: ", (bi - c) / 48.0);
                  int shown = 0; for (int i = c + 1; i < c + 9600 && shown < 12; ++i) if (std::abs (out[(size_t) i] - out[(size_t) i - 1]) > 0.08f) { std::printf ("%.1f ", (i - c) / 48.0); ++shown; }
                  std::printf ("ms\n"); }
            }
        }
        // 2d) the vessel: a file looped between two points, at full level, seam without clicks
        {
            AudioProcessor::setTypeOfNextNewPlugin (AudioProcessor::wrapperType_Standalone);
            ChloaelisProcessor p;
            AudioProcessor::setTypeOfNextNewPlugin (AudioProcessor::wrapperType_Undefined);
            p.prepareToPlay (48000, 512);
            const bool ok = p.loadVesselFile (File ("~/chloe/chloaelis_demo_tape.wav").existsAsFile() ? File ("~/chloe/chloaelis_demo_tape.wav")
                                                                                                : File::getSpecialLocation (File::userHomeDirectory).getChildFile ("chloe/chloaelis_demo_tape.wav"));
            const bool whole = std::getenv ("CHL_VESSEL_WHOLE") != nullptr;
            p.vesselStart = whole ? 0.0f : 0.2f; p.vesselEnd = whole ? 1.0f : 0.35f; p.vesselLevel = 1.0f;
            p.apvts.getParameter (chl::pid (0, chl::wet))->setValueNotifyingHost (0.0f);
            p.apvts.getParameter (chl::pid (1, chl::wet))->setValueNotifyingHost (0.0f);
            AudioBuffer<float> b (2, 512); MidiBuffer m; double e = 0; int cnt = 0; float jump = 0, prev = 0;
            for (int blk = 0; blk < 400; ++blk)
            {
                b.clear(); p.processBlock (b, m);
                for (int i = 0; i < 512; ++i) { const float y = b.getSample (0, i); if (blk > 20) { e += y * y; ++cnt; jump = jmax (jump, std::abs (y - prev)); } prev = y; }
            }
            std::printf ("vessel: loaded %s, loop 20-35%% at full level: %.1f dB rms, largest step %.3f\n", ok ? "yes" : "NO", 10.0 * std::log10 (e / jmax (1, cnt) + 1e-12), jump);
        }
        // 3) master feedback flat out with regen up: bounded and finite?
        {
            ChloaelisProcessor p;
            p.prepareToPlay (48000, 512);
            p.apvts.getParameter (chl::masterFbId)->setValueNotifyingHost (1.0f);
            p.apvts.getParameter (chl::pid (0, chl::regen))->setValueNotifyingHost (1.0f);
            p.apvts.getParameter (chl::pid (1, chl::drive))->setValueNotifyingHost (1.0f);
            AudioBuffer<float> b (2, 512); MidiBuffer m; float peak = 0; bool fin = true; Random rnd (3);
            for (int blk = 0; blk < 2000; ++blk)
            {
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) b.setSample (c, i, blk < 20 ? rnd.nextFloat() * 0.5f - 0.25f : 0.0f);
                p.processBlock (b, m);
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) { const float y = b.getSample (c, i); fin = fin && std::isfinite (y); peak = jmax (peak, std::abs (y)); }
            }
            std::printf ("master feedback 100%%, regen max: peak %.2f, finite %s\n", peak, fin ? "yes" : "NO");
        }
        // 4) through the processor: the source at full level reaches both outputs
        {
            ChloaelisProcessor p;
            p.prepareToPlay (48000, 512);
            p.apvts.getParameter (chl::srcLevelId)->setValueNotifyingHost (1.0f);
            Thread::sleep (300);   // let the background thread fill
            AudioBuffer<float> b (2, 512); MidiBuffer m; double e[2] {};
            for (int blk = 0; blk < 60; ++blk)
            {
                b.clear(); p.processBlock (b, m);
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) e[c] += b.getSample (c, i) * b.getSample (c, i);
                Thread::sleep (5);
            }
            std::printf ("source through the plugin: L %.1f dB  R %.1f dB\n", 10.0 * std::log10 (e[0] / (60 * 512) + 1e-12), 10.0 * std::log10 (e[1] / (60 * 512) + 1e-12));
        }
        return 0;
    }

    if (std::getenv ("CHL_LIVE") != nullptr)
    {
        ChloaelisProcessor p;
        p.prepareToPlay (48000, 512);
        p.apvts.getParameter (chl::lzDepthId)->setValueNotifyingHost (0.5f);
        p.apvts.getParameter (chl::lzSpeedId)->setValueNotifyingHost (0.7f);
        p.apvts.getParameter (chl::pid (0, chl::lmDepth))->setValueNotifyingHost (0.4f);
        p.apvts.getParameter (chl::widthId)->setValueNotifyingHost (0.5f);
        p.setPlayConfigDetails (2, 16, 48000, 512);
        p.prepareToPlay (48000, 512);
        p.apvts.getParameter (chl::flockId)->setValueNotifyingHost (0.8f);
        p.apvts.getParameter (chl::widthId)->setValueNotifyingHost (1.0f);
        DocumentWindow win ("Chloaeilius fps", Colours::black, 0);
        win.setUsingNativeTitleBar (true);
        auto* ed = dynamic_cast<ChloaelisEditor*> (p.createEditorAndMakeActive());
        win.setContentOwned (ed, true);
        win.setVisible (true);
        win.toFront (true);
        AudioBuffer<float> buf (16, 512); MidiBuffer m;
        int blk = 0;
        auto run = [&] (bool sweep, const char* what)
        {
            MessageManager::getInstance()->runDispatchLoopUntil (600);   // settle
            const int f0 = chloaelisDbgFrames;
            const double t0 = Time::getMillisecondCounterHiRes();
            while (Time::getMillisecondCounterHiRes() - t0 < 3000.0)
            {
                const double el = (Time::getMillisecondCounterHiRes() - t0) * 0.001;
                ed->holdFocusForTest (sweep ? 410.0f + 380.0f * (float) std::sin (el * 2.0) : -1.0f);
                // the cursor wanders across the whole window, so the camera keeps swinging
                ed->holdMouseForTest (sweep ? 600.0f + 560.0f * (float) std::sin (el * 1.3) : 160.0f, sweep ? 410.0f + 380.0f * (float) std::sin (el * 2.0) : 400.0f);
                for (int i = 0; i < 512; ++i) for (int c = 0; c < 2; ++c) buf.setSample (c, i, std::sin ((blk * 512 + i) * 0.02f) * 0.4f);
                ++blk; p.processBlock (buf, m);
                MessageManager::getInstance()->runDispatchLoopUntil (10);
            }
            const double secs = (Time::getMillisecondCounterHiRes() - t0) * 0.001;
            const double fps = (chloaelisDbgFrames - f0) / secs;
            std::printf ("live fps (%s): %.1f\n", what, fps);
            return fps;
        };
        // the views behind the knobs share the frame with the room's depth-of-field blur (Chloaelis itself
        // measures 34 to 41 here); the big sphere, alone on screen, should be smooth
        double worstRoom = 1e9, worstBig = 1e9;
        static const char* views[] = { "lorenz view", "orrery view", "map view", "radar view" };
        for (int v = 0; v < 4; ++v)
        {
            p.apvts.getParameter (chl::viewId)->setValueNotifyingHost ((float) v / 3.0f);
            worstRoom = jmin (worstRoom, run (v == 1, views[v]));
        }
        for (int v = 1; v <= 3; ++v)
        {
            ed->showSphereForTest (v, 0.4f * (float) v, 0.2f);
            static const char* big[] = { "", "big sphere: globe", "big sphere: map", "big sphere: radar" };
            worstBig = jmin (worstBig, run (false, big[v]));
        }
        ed->showSphereForTest (0);
        win.clearContentComponent();
        const bool ok = worstRoom >= 27.0 && worstBig >= 50.0;
        std::printf ("%s (room views worst %.1f, big sphere worst %.1f)\n", ok ? "LIVE FPS OK" : "LIVE FPS TOO LOW", worstRoom, worstBig);
        return ok ? 0 : 1;
    }

    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        ChloaelisProcessor p;
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        // push things hard, and keep moving parameters while audio runs
        p.apvts.getParameter (chl::pid (0, chl::regen))->setValueNotifyingHost (1.0f);
        p.apvts.getParameter (chl::pid (1, chl::drive))->setValueNotifyingHost (1.0f);
        Random rnd (42);
        AudioBuffer<float> buf (2, 4096);
        MidiBuffer midi;
        double peak = 0; bool finite = true;
        const int sizes[] = { 1, 7, 64, 333, 512, 1024, 4096, 13 };
        for (int it = 0; it < 500; ++it)
        {
            const int n = sizes[it % 8];
            buf.setSize (2, n, false, false, true);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i) buf.setSample (c, i, rnd.nextFloat() * 0.4f - 0.2f);
            if (it % 45 == 0) p.apvts.getParameter (chl::tubeOnId (it / 45 % 2))->setValueNotifyingHost ((it / 90) % 2 ? 0.0f : 1.0f);
            if (it % 9 == 0)
                for (int sd = 0; sd < 2; ++sd)
                    p.apvts.getParameter (chl::pid (sd, (it / 9) % chl::numMod))->setValueNotifyingHost (rnd.nextFloat());
            if (it % 60 == 0) p.apvts.getParameter (chl::freezeId (it / 60 % 2))->setValueNotifyingHost ((it / 120) % 2 ? 1.0f : 0.0f);
            // engines, slew/fade, order, poly width, lorenz, LFO depths, random freeze
            if (it % 33 == 0) p.apvts.getParameter (chl::delayTypeId (it / 33 % 2))->setValueNotifyingHost (rnd.nextFloat());
            if (it % 41 == 0) p.apvts.getParameter (chl::timeModeId (it / 41 % 2))->setValueNotifyingHost ((it / 82) % 2 ? 1.0f : 0.0f);
            if (it % 77 == 0) p.apvts.getParameter (chl::orderId)->setValueNotifyingHost ((it / 77) % 2 ? 1.0f : 0.0f);
            if (it % 25 == 0) p.apvts.getParameter (chl::widthId)->setValueNotifyingHost (rnd.nextFloat());
            if (it % 50 == 0) for (auto* id : { &chl::lzDepthId, &chl::lzSpeedId, &chl::lzChaosId, &chl::lzShapeId })
                p.apvts.getParameter (*id)->setValueNotifyingHost (rnd.nextFloat());
            if (it % 30 == 0) p.refreshTape[(size_t) (it / 30 % 2)] = true;
            p.processBlock (buf, midi);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i)
            { const float v = buf.getSample (c, i); if (! std::isfinite (v)) finite = false; peak = jmax (peak, (double) std::abs (v)); }
        }
        std::printf ("sr %6.0f: finite=%d peak=%.2f\n", sr, finite, peak);
        if (! finite || peak > 10.0) ++fails;   // +12 dB in and out, all modulated: loud but sane
    }

    // the two sides stay apart: the left input alone (echoes off) comes from the left of the sphere,
    // where the left side's dry sound stands (ACN 1, Y, is the left-right channel)
    {
        ChloaelisProcessor p;
        p.setPlayConfigDetails (2, 16, 48000, 256);
        p.prepareToPlay (48000, 256);
        for (int s = 0; s < 2; ++s) p.apvts.getParameter (chl::pid (s, chl::wet))->setValueNotifyingHost (0.0f);
        AudioBuffer<float> buf (16, 256); MidiBuffer m;
        double w = 0, y = 0;
        for (int b = 0; b < 400; ++b)
        {
            buf.clear();
            for (int i = 0; i < 256; ++i) buf.setSample (0, i, std::sin ((b * 256 + i) * 0.03f) * 0.3f);
            p.processBlock (buf, m);
            if (b > 100) { w += buf.getRMSLevel (0, 0, 256); y += buf.getRMSLevel (1, 0, 256); }
        }
        std::printf ("left input -> left of the sphere: W %.3f  Y %.3f\n", w / 300, y / 300);
        if (w / 300 < 0.01 || y / w < 0.6) ++fails;
    }

    // mono in / 16 out (3rd-order Ambisonics)
    {
        ChloaelisProcessor p;
        AudioProcessor::BusesLayout lay; lay.inputBuses.add (AudioChannelSet::mono()); lay.outputBuses.add (AudioChannelSet::ambisonic (3));
        const bool ok = p.setBusesLayout (lay);
        std::printf ("mono -> 3rd-order ambisonic layout supported: %d\n", ok);
        if (! ok) ++fails;
    }

    // state round trip
    {
        ChloaelisProcessor a, b;
        a.apvts.getParameter (chl::pid (0, chl::cutoff))->setValueNotifyingHost (0.3f);
        a.apvts.getParameter (chl::pid (1, chl::time))->setValueNotifyingHost (0.7f);
        MemoryBlock mb; a.getStateInformation (mb);
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        bool same = true;
        for (auto pidStr : { chl::pid (0, chl::cutoff), chl::pid (1, chl::time) })
            same &= std::abs (a.apvts.getParameter (pidStr)->getValue() - b.apvts.getParameter (pidStr)->getValue()) < 1e-6f;
        // tape wear survives a save / load
        {
            ChloaelisProcessor c, d2;
            c.prepareToPlay (48000, 512);
            c.apvts.getParameter (chl::delayTypeId (0))->setValueNotifyingHost (0.0f);
            AudioBuffer<float> bb (2, 512); MidiBuffer mm; bb.clear();
            for (int i = 0; i < 2000; ++i) c.processBlock (bb, mm);   // ~21 s of tape
            MemoryBlock m2; c.getStateInformation (m2);
            d2.setStateInformation (m2.getData(), (int) m2.getSize());
            d2.prepareToPlay (48000, 512);
            d2.processBlock (bb, mm);
            const float a1 = c.tapeAge[0].load(), a2 = d2.tapeAge[0].load();
            std::printf ("tape age %.4f -> reloaded %.4f\n", a1, a2);
            same &= a1 > 0.02f && std::abs (a1 - a2) < 0.002f;
        }
        std::printf ("state round trip: %d\n", same);
        if (! same) ++fails;
    }

    // UI screenshot (with some modulation visible)
    {
        AudioProcessor::setTypeOfNextNewPlugin (AudioProcessor::wrapperType_Standalone);   // so the desktop-only parts show
        ChloaelisProcessor p;
        AudioProcessor::setTypeOfNextNewPlugin (AudioProcessor::wrapperType_Undefined);
        p.prepareToPlay (48000, 512);
        p.apvts.getParameter (chl::freezeId (0))->setValueNotifyingHost (1.0f);
        p.apvts.getParameter (chl::tubeOnId (1))->setValueNotifyingHost (0.0f);
        p.apvts.getParameter (chl::delayTypeId (0))->setValueNotifyingHost (0.0f);   // tape
        p.apvts.getParameter (chl::delayTypeId (1))->setValueNotifyingHost (1.0f);   // BBD
        p.apvts.getParameter (chl::timeModeId (1))->setValueNotifyingHost (1.0f);
        p.apvts.getParameter (chl::widthId)->setValueNotifyingHost (0.45f);
        p.apvts.getParameter (chl::lzDepthId)->setValueNotifyingHost (0.4f);
        p.apvts.getParameter (chl::lzSpeedId)->setValueNotifyingHost (0.75f);
        p.apvts.getParameter (chl::pid (0, chl::lmDepth))->setValueNotifyingHost (0.3f);
        p.apvts.getParameter (chl::pid (1, chl::rnd))->setValueNotifyingHost (0.5f);
        p.apvts.getParameter (chl::flockId)->setValueNotifyingHost (0.8f);
        AudioBuffer<float> buf (2, 512); MidiBuffer m;
        for (int b = 0; b < 60; ++b) { for (int i = 0; i < 512; ++i) for (int c = 0; c < 2; ++c) buf.setSample (c, i, std::sin ((b * 512 + i) * 0.02f) * 0.4f); p.processBlock (buf, m); }
        p.loadVesselFile (File::getSpecialLocation (File::userHomeDirectory).getChildFile ("chloe/chloaelis_demo_tape.wav"));
        p.vesselStart = 0.25f; p.vesselEnd = 0.7f; p.vesselLevel = 0.5f;
        std::unique_ptr<AudioProcessorEditor> ed (p.createEditorAndMakeActive());
        ed->setSize (ChloaelisEditor::designW, ChloaelisEditor::designH);
        File out (argc > 1 ? argv[1] : "chloaelis_ui.png");
        if (auto* dn = std::getenv ("CHL_DENSITY"))
            if (auto* ce = dynamic_cast<ChloaelisEditor*> (ed.get())) ce->setDensityForTest ((float) std::atof (dn));
        for (int shot = 0; shot < 5; ++shot)
        {
            // centred; focus low; cursor at the left; cursor at the right; silence (cool palette)
            const float fy = shot == 1 ? 700.0f : -1.0f;
            if (auto* ce = dynamic_cast<ChloaelisEditor*> (ed.get()))
            {
                if (shot == 2)      ce->holdMouseForTest (170.0f, 330.0f);
                else if (shot == 3) ce->holdMouseForTest (1040.0f, 520.0f);
                else                ce->holdMouseForTest (-1.0f, -1.0f);
                if (shot == 4)
                    for (int b = 0; b < 40; ++b) { buf.clear(); p.processBlock (buf, m); ce->advanceFrames (4, fy); }
                ce->advanceFrames (150, fy);
            }
            // keep audio running so the attractor and levels move
            for (int b = 0; b < 30; ++b) { for (int i = 0; i < 512; ++i) for (int c = 0; c < 2; ++c) buf.setSample (c, i, shot == 4 ? 0.0f : std::sin ((b * 512 + i) * 0.02f) * 0.4f); p.processBlock (buf, m);
                                           if (auto* ce = dynamic_cast<ChloaelisEditor*> (ed.get())) ce->advanceFrames (1, fy); }
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            static const char* suffix[] = { "", "_bottom", "_left", "_right", "_quiet" };
            auto f = shot == 0 ? out : out.getSiblingFile (out.getFileNameWithoutExtension() + suffix[shot] + ".png");
            f.deleteFile();
            FileOutputStream os (f);
            PNGImageFormat().writeImageToStream (img, os);
            std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
        }
        {
            // a screen of another shape (16:10): the field should reach every edge
            ed->setSize (1512, 945);
            if (auto* ce = dynamic_cast<ChloaelisEditor*> (ed.get())) ce->advanceFrames (20);
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            auto f = out.getSiblingFile (out.getFileNameWithoutExtension() + "_wide.png");
            f.deleteFile(); FileOutputStream os (f); PNGImageFormat().writeImageToStream (img, os);
            std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
        }
        if (auto* ce = dynamic_cast<ChloaelisEditor*> (ed.get()))
        {
            ed->setSize (ChloaelisEditor::designW, ChloaelisEditor::designH);
            ce->showHelpForTest (true);
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            auto f = out.getSiblingFile (out.getFileNameWithoutExtension() + "_help.png");
            f.deleteFile(); FileOutputStream os (f); PNGImageFormat().writeImageToStream (img, os);
            std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
            ce->showHelpForTest (false);
        }
        if (auto* ce = dynamic_cast<ChloaelisEditor*> (ed.get()))
        {
            // the Brick theme, with the left side on Karplus-Strong time
            p.uiTheme = 1;
            p.apvts.getParameter (chl::ksId (0))->setValueNotifyingHost (1.0f);
            for (int b = 0; b < 30; ++b) { for (int i = 0; i < 512; ++i) for (int c = 0; c < 2; ++c) buf.setSample (c, i, std::sin ((b * 512 + i) * 0.02f) * 0.4f); p.processBlock (buf, m); ce->advanceFrames (2); }
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            auto f = out.getSiblingFile (out.getFileNameWithoutExtension() + "_brick.png");
            f.deleteFile(); FileOutputStream os (f); PNGImageFormat().writeImageToStream (img, os);
            std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
            p.uiTheme = 0;
            p.apvts.getParameter (chl::ksId (0))->setValueNotifyingHost (0.0f);
            ce->advanceFrames (30);
            img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            f = out.getSiblingFile (out.getFileNameWithoutExtension() + "_back.png");
            f.deleteFile(); FileOutputStream os2 (f); PNGImageFormat().writeImageToStream (img, os2);
        }
        ed.reset();
    }

    std::printf (fails ? "HOST TEST PROBLEMS\n" : "HOST TEST OK\n");
    return fails;
}
