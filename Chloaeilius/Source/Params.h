#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// Parameter ids and layout for Chloaelis: two Chloe-style channel strips (L / R, hard panned)
// plus a shared Lorenz modulator, the poly width and the filter / delay order.
namespace chl
{
    enum ModParam { inAtt, cutoff, res, morph, drive, time, regen, wet, out,
                    rnd, lmRate, lmDepth, ltRate, ltDepth, ldRate, ldDepth, numMod };
    constexpr int numSides = 2;

    inline const char* const modKeys[numMod]  = { "inatt", "cutoff", "res", "morph", "drive", "time", "regen", "wet", "out",
                                                  "randfrz", "lfomode_rate", "lfomode_depth", "lfotime_rate", "lfotime_depth", "lfodrive_rate", "lfodrive_depth" };
    inline const char* const modNames[numMod] = { "In Att", "Cutoff", "Resonance", "Mode", "Drive", "Time", "Regen", "Wet", "Out",
                                                  "Random Freeze", "LFO>Mode Rate", "LFO>Mode Depth", "LFO>Time Rate", "LFO>Time Depth", "LFO>Drive Rate", "LFO>Drive Depth" };

    inline juce::String sideTag (int side)            { return side == 0 ? "l_" : "r_"; }
    inline juce::String sideName (int side)           { return side == 0 ? "L " : "R "; }
    inline juce::String pid (int side, int p)         { return sideTag (side) + modKeys[p]; }
    inline juce::String hpfId (int side)              { return sideTag (side) + "hpf"; }
    inline juce::String freezeId (int side)           { return sideTag (side) + "freeze"; }
    inline juce::String tubeOnId (int side)           { return sideTag (side) + "tubeon"; }
    inline juce::String delayTypeId (int side)        { return sideTag (side) + "dtype"; }
    inline juce::String timeModeId (int side)         { return sideTag (side) + "tmode"; }
    inline juce::String tubeTypeId (int side)         { return sideTag (side) + "tubetype"; }

    // shared
    inline const juce::String lzSpeedId = "lz_speed", lzChaosId = "lz_chaos", lzShapeId = "lz_shape", lzDepthId = "lz_depth",
                              widthId = "width", orderId = "order",
                              srcSelId = "src_sel", srcLevelId = "src_level", masterFbId = "master_fb", flockId = "poly_flock",
                              spaceId = "space",   // the size of the bubble the echoes fly in (1.5 m .. 24 m)
                              viewId = "space_view",   // how the sphere is shown (no effect on the sound)
                              listenId = "listen";      // the desktop app only: headphones, speakers, or the 16 channels themselves

    inline bool isLfoRate (int p) { return p == lmRate || p == ltRate || p == ldRate; }

    inline juce::NormalisableRange<float> rangeFor (int p)
    {
        using R = juce::NormalisableRange<float>;
        switch (p)
        {
            case inAtt:  return R (-48.0f, 12.0f, 0.01f);
            case cutoff: { R r (20.0f, 20000.0f, 0.01f); r.setSkewForCentre (632.0f); return r; }
            case morph:  return R (0.0f, 3.0f, 0.0001f);
            case time:   { R r (0.03f, 1.0f, 0.0001f); r.setSkewForCentre (0.25f); return r; }
            case regen:  return R (0.0f, 1.15f, 0.0001f);
            case out:    return R (-48.0f, 12.0f, 0.01f);
            case lmRate: case ltRate: case ldRate: { R r (0.02f, 20.0f, 0.0001f); r.setSkewForCentre (1.0f); return r; }
            default:     return R (0.0f, 1.0f, 0.0001f);
        }
    }

    inline float defaultFor (int p, int side)
    {
        switch (p)
        {
            case cutoff: return 20000.0f;
            case res:    return 0.15f;
            case drive:  return 0.3f;
            case time:   return side == 0 ? 0.33f : 0.41f;
            case regen:  return 0.4f;
            case wet:    return 0.35f;
            case lmRate: return side == 0 ? 0.21f : 0.17f;
            case ltRate: return side == 0 ? 0.11f : 0.13f;
            case ldRate: return side == 0 ? 0.37f : 0.29f;
            default:     return 0.0f;
        }
    }

    inline juce::String unitFor (int p)
    {
        switch (p)
        {
            case inAtt: case out: return " dB";
            case cutoff:          return " Hz";
            default:              return {};
        }
    }

    inline juce::String hzText (float v) { return v < 1.0f ? juce::String (v, 2) + " Hz" : juce::String (v, 1) + " Hz"; }

    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;

        for (int s = 0; s < numSides; ++s)
        {
            auto grp = std::make_unique<AudioProcessorParameterGroup> (sideTag (s) + "grp", s == 0 ? "Left" : "Right", "|");
            for (int p = 0; p < numMod; ++p)
            {
                const int pp = p;
                grp->addChild (std::make_unique<AudioParameterFloat> (
                    ParameterID { pid (s, p), 1 }, sideName (s) + modNames[p], rangeFor (p), defaultFor (p, s),
                    AudioParameterFloatAttributes().withLabel (unitFor (p)).withStringFromValueFunction (
                        [pp] (float v, int) -> String
                        {
                            if (pp == inAtt || pp == out) return v <= -47.9f ? String ("-inf") : String (std::abs (v) < 0.05f ? 0.0f : v, 1);
                            if (pp == cutoff)             return v >= 1000.0f ? String (v / 1000.0f, 1) + "k" : String ((int) v);
                            if (pp == time)               return String (v * 1000.0f, 0) + " ms";
                            if (isLfoRate (pp))           return hzText (v);
                            if (pp == morph)
                            {
                                static const char* n[] = { "LP", "BP", "HP", "Notch" };
                                const int a = jlimit (0, 3, (int) v);
                                const float f = v - (float) a;
                                return f < 0.02f || a == 3 ? String (n[a]) : String (n[a]) + ">" + n[a + 1] + " " + String ((int) (f * 100)) + "%";
                            }
                            return String ((int) std::round (v * 100.0f)) + "%";
                        })));
            }
            grp->addChild (std::make_unique<AudioParameterBool> (ParameterID { hpfId (s), 1 }, sideName (s) + "Delay HPF", false));
            grp->addChild (std::make_unique<AudioParameterBool> (ParameterID { freezeId (s), 1 }, sideName (s) + "Freeze", false));
            grp->addChild (std::make_unique<AudioParameterBool> (ParameterID { tubeOnId (s), 1 }, sideName (s) + "Tube On", true));
            grp->addChild (std::make_unique<AudioParameterChoice> (ParameterID { delayTypeId (s), 1 }, sideName (s) + "Delay Type", StringArray { "Tape", "PT2399", "BBD" }, 1));
            grp->addChild (std::make_unique<AudioParameterChoice> (ParameterID { timeModeId (s), 1 }, sideName (s) + "Time Change", StringArray { "Slew", "Fade" }, 0));
            grp->addChild (std::make_unique<AudioParameterChoice> (ParameterID { tubeTypeId (s), 1 }, sideName (s) + "Tube Type", StringArray { "12AX7", "EL84", "6L6", "Starved" }, 0));
            layout.add (std::move (grp));
        }

        auto g = std::make_unique<AudioProcessorParameterGroup> ("shared", "Shared", "|");
        auto pct = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String ((int) std::round (v * 100.0f)) + "%"; });
        g->addChild (std::make_unique<AudioParameterFloat> (ParameterID { lzSpeedId, 1 }, "Lorenz Speed", NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.35f, pct));
        g->addChild (std::make_unique<AudioParameterFloat> (ParameterID { lzChaosId, 1 }, "Lorenz Chaos", NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.3f,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String::fromUTF8 ("\xcf\x81 ") + String (24.5f + v * v * 155.5f, 1); })));
        g->addChild (std::make_unique<AudioParameterFloat> (ParameterID { lzShapeId, 1 }, "Lorenz Shape", NormalisableRange<float> (0.5f, 4.0f, 0.0001f), 8.0f / 3.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String::fromUTF8 ("\xce\xb2 ") + String (v, 2); })));
        g->addChild (std::make_unique<AudioParameterFloat> (ParameterID { lzDepthId, 1 }, "Lorenz > Cutoff", NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f, pct));
        g->addChild (std::make_unique<AudioParameterFloat> (ParameterID { widthId, 1 }, "Poly Width", NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v < 0.0005f ? String ("OFF") : String ((int) std::round (v * 100.0f)) + "%"; })));
        g->addChild (std::make_unique<AudioParameterFloat> (ParameterID { flockId, 1 }, "Poly Flock", NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f, pct));
        g->addChild (std::make_unique<AudioParameterChoice> (ParameterID { orderId, 1 }, "Order", StringArray { "Filter > Delay", "Delay > Filter" }, 0));
        // the built-in recording: which version, and how loud (fade it in and out)
        g->addChild (std::make_unique<AudioParameterChoice> (ParameterID { srcSelId, 1 }, "Source", StringArray { "Base Loop", "Hydrophone" }, 0));
        g->addChild (std::make_unique<AudioParameterFloat> (ParameterID { srcLevelId, 1 }, "Source Level", NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f, pct));
        g->addChild (std::make_unique<AudioParameterFloat> (ParameterID { masterFbId, 1 }, "Master Feedback", NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f, pct));
        g->addChild (std::make_unique<AudioParameterFloat> (ParameterID { spaceId, 1 }, "Distance", NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.6f,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (1.5f * std::pow (16.0f, v), 1) + " m"; })));
        g->addChild (std::make_unique<AudioParameterChoice> (ParameterID { listenId, 1 }, "Listen (desktop app)", StringArray { "Headphones", "Speakers", "16 ch" }, 0));
        g->addChild (std::make_unique<AudioParameterChoice> (ParameterID { viewId, 1 }, "Space View", StringArray { "Lorenz", "Orrery", "Map", "Radar" }, 1));
        layout.add (std::move (g));

        return layout;
    }
}
