#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace fp
{
struct ScaleDegree
{
    double cents;
    std::string label;
};

struct Scale
{
    std::string name;
    std::vector<ScaleDegree> degrees; // within one period, ascending (sorted at build time)
    double period = 1200.0;           // cents (octave = 1200, tritave = 1901.955)
};

namespace scales_detail
{
    inline double ratioCents (double n, double d) { return 1200.0 * std::log2 (n / d); }

    inline ScaleDegree r (int n, int d)
    {
        return { ratioCents (n, d), std::to_string (n) + "/" + std::to_string (d) };
    }

    inline ScaleDegree c (double cents, const char* label = nullptr)
    {
        if (label != nullptr) return { cents, label };
        char tmp[32];
        std::snprintf (tmp, sizeof (tmp), "%.0fc", cents);
        return { cents, tmp };
    }

    inline Scale edo (const std::string& name, int steps, double period = 1200.0)
    {
        Scale s { name, {}, period };
        for (int i = 0; i < steps; ++i)
        {
            char tmp[32];
            std::snprintf (tmp, sizeof (tmp), "%d\\%d", i, steps);
            s.degrees.push_back ({ period * i / steps, tmp });
        }
        return s;
    }
}

// All scales offered by the pitch shifter. The shift knob is hard-quantised to
// the nearest degree (in any octave/period) of the selected scale.
inline const std::vector<Scale>& getScales()
{
    using namespace scales_detail;
    static const std::vector<Scale> list = []
    {
        std::vector<Scale> v;

        // La Monte Young - The Well-Tuned Piano (1964-), 7-limit just intonation.
        // Keys Eb..D map to these ratios (note G# sits *below* G, and C# below C).
        v.push_back ({ "The Well-Tuned Piano (La Monte Young)",
                       { r (1, 1), r (567, 512), r (9, 8), r (147, 128), r (1323, 1024), r (21, 16),
                         r (189, 128), r (3, 2), r (49, 32), r (441, 256), r (7, 4), r (63, 32) } });

        // Hindustani ragas in just intonation (Sa = 1/1)
        v.push_back ({ "Raga Bhairav",     { r (1,1), r (16,15), r (5,4), r (4,3), r (3,2), r (8,5), r (15,8) } });
        v.push_back ({ "Raga Yaman",       { r (1,1), r (9,8), r (5,4), r (45,32), r (3,2), r (27,16), r (15,8) } });
        v.push_back ({ "Raga Bhairavi",    { r (1,1), r (16,15), r (6,5), r (4,3), r (3,2), r (8,5), r (9,5) } });
        v.push_back ({ "Raga Todi",        { r (1,1), r (16,15), r (6,5), r (45,32), r (3,2), r (8,5), r (15,8) } });
        v.push_back ({ "Raga Marwa",       { r (1,1), r (16,15), r (5,4), r (45,32), r (27,16), r (15,8) } });
        v.push_back ({ "Raga Purvi",       { r (1,1), r (16,15), r (5,4), r (45,32), r (3,2), r (8,5), r (15,8) } });
        v.push_back ({ "Raga Kafi",        { r (1,1), r (9,8), r (6,5), r (4,3), r (3,2), r (5,3), r (9,5) } });
        v.push_back ({ "Raga Malkauns",    { r (1,1), r (6,5), r (4,3), r (8,5), r (9,5) } });
        v.push_back ({ "Raga Darbari Kanada", { r (1,1), r (9,8), r (32,27), r (4,3), r (3,2), r (128,81), r (16,9) } });
        v.push_back ({ "Raga Bhupali",     { r (1,1), r (9,8), r (5,4), r (3,2), r (5,3) } });
        v.push_back ({ "Raga Desh",        { r (1,1), r (9,8), r (5,4), r (4,3), r (3,2), r (27,16), r (16,9), r (15,8) } });
        v.push_back ({ "Raga Puriya Dhanashri", { r (1,1), r (16,15), r (5,4), r (45,32), r (3,2), r (8,5), r (15,8) } });

        // 22 shruti (Indian microtonal gamut)
        v.push_back ({ "22 Shruti",
                       { r (1,1), r (256,243), r (16,15), r (10,9), r (9,8), r (32,27), r (6,5), r (5,4),
                         r (81,64), r (4,3), r (27,20), r (45,32), r (729,512), r (3,2), r (128,81),
                         r (8,5), r (5,3), r (27,16), r (16,9), r (9,5), r (15,8), r (243,128) } });

        // Maqamat (quarter-tone)
        v.push_back ({ "Maqam Rast",   { c (0), c (200), c (350), c (500), c (700), c (900), c (1050) } });
        v.push_back ({ "Maqam Bayati", { c (0), c (150), c (300), c (500), c (700), c (800), c (1000) } });
        v.push_back ({ "Maqam Saba",   { c (0), c (150), c (300), c (400), c (600), c (800), c (1000) } });

        // Gamelan (measured-style approximations)
        v.push_back ({ "Slendro", { c (0), c (231), c (474), c (717), c (955) } });
        v.push_back ({ "Pelog",   { c (0), c (120), c (270), c (540), c (670), c (785), c (950) } });

        // Just / harmonic
        v.push_back ({ "Harmonic Series 8-16",
                       { r (8,8), r (9,8), r (10,8), r (11,8), r (12,8), r (13,8), r (14,8), r (15,8) } });

        v.push_back ({ "Partch 43-Tone",
                       { r (1,1), r (81,80), r (33,32), r (21,20), r (16,15), r (12,11), r (11,10), r (10,9),
                         r (9,8), r (8,7), r (7,6), r (32,27), r (6,5), r (11,9), r (5,4), r (14,11), r (9,7),
                         r (21,16), r (4,3), r (27,20), r (11,8), r (7,5), r (10,7), r (16,11), r (40,27),
                         r (3,2), r (32,21), r (14,9), r (11,7), r (8,5), r (18,11), r (5,3), r (27,16),
                         r (12,7), r (7,4), r (16,9), r (9,5), r (20,11), r (11,6), r (15,8), r (40,21),
                         r (64,33), r (160,81) } });

        // Bohlen-Pierce: repeats at the tritave (3/1), not the octave
        {
            Scale bp { "Bohlen-Pierce (tritave)",
                       { r (1,1), r (27,25), r (25,21), r (9,7), r (7,5), r (75,49), r (5,3),
                         r (9,5), r (49,25), r (15,7), r (7,3), r (63,25), r (25,9) },
                       ratioCents (3, 1) };
            v.push_back (bp);
        }

        // Equal divisions
        v.push_back (edo ("Quarter-tone (24-EDO)", 24));
        v.push_back (edo ("19-EDO", 19));
        v.push_back (edo ("31-EDO", 31));
        v.push_back (edo ("Chromatic (12-EDO)", 12));

        for (auto& s : v)
            std::sort (s.degrees.begin(), s.degrees.end(),
                       [] (const ScaleDegree& a, const ScaleDegree& b) { return a.cents < b.cents; });
        return v;
    }();
    return list;
}

struct Quantised
{
    double cents = 0.0;
    int degreeIndex = 0;
    int period = 0;
};

// Snap an interval (cents) to the closest degree of the scale in any period.
inline Quantised quantise (const Scale& s, double cents)
{
    Quantised best;
    double bestDist = 1.0e9;
    const int base = (int) std::floor (cents / s.period);
    for (int p = base - 1; p <= base + 1; ++p)
        for (int i = 0; i < (int) s.degrees.size(); ++i)
        {
            const double cand = p * s.period + s.degrees[(size_t) i].cents;
            const double d = std::abs (cand - cents);
            if (d < bestDist) { bestDist = d; best = { cand, i, p }; }
        }
    return best;
}

//==============================================================================
// Delay tap ratios. Starting at the degree the pitch shifter landed on, stack the
// next scale degrees (wrapping into the next period): left taps use steps 0, 2, 4,
// right taps use steps 1, 3, 5. Each ratio is relative to the landed degree, so
// the first left tap is exactly the base time and the rest are just-intonation
// multiples of it (or the tempered equivalent for cents-based scales).
struct TapRatios
{
    double ratio[6] {};     // [L0, R0, L1, R1, L2, R2] = steps 0..5
    std::string label[6];
};

namespace scales_detail
{
    inline bool parseFraction (const std::string& s, long long& n, long long& d)
    {
        const auto slash = s.find ('/');
        if (slash == std::string::npos) return false;
        try { n = std::stoll (s.substr (0, slash)); d = std::stoll (s.substr (slash + 1)); }
        catch (...) { return false; }
        return n > 0 && d > 0;
    }
    inline long long gcd (long long a, long long b) { while (b) { const long long t = a % b; a = b; b = t; } return a; }
}

inline TapRatios delayTapRatios (const Scale& s, int degreeIndex)
{
    using namespace scales_detail;
    TapRatios out;
    const int n = (int) s.degrees.size();
    const int d = std::clamp (degreeIndex, 0, n - 1);
    const double base = s.degrees[(size_t) d].cents;

    long long periodNum = 0;
    if (std::abs (s.period - 1200.0) < 0.01) periodNum = 2;
    else if (std::abs (s.period - ratioCents (3, 1)) < 0.01) periodNum = 3;

    long long bn = 0, bd = 0;
    const bool baseExact = parseFraction (s.degrees[(size_t) d].label, bn, bd);

    for (int k = 0; k < 6; ++k)
    {
        const int idx = d + k;
        const int per = idx / n, i = idx % n;
        const double cents = per * s.period + s.degrees[(size_t) i].cents - base;
        out.ratio[k] = std::pow (2.0, cents / 1200.0);

        long long a = 0, b = 0;
        bool exact = baseExact && periodNum > 0 && parseFraction (s.degrees[(size_t) i].label, a, b);
        if (exact)
        {
            long long num = a * bd, den = b * bn;
            for (int p = 0; p < per; ++p) num *= periodNum;
            const long long g = gcd (num, den);
            num /= g; den /= g;
            exact = num < 100000 && den < 100000;
            if (exact) out.label[k] = std::to_string (num) + "/" + std::to_string (den);
        }
        if (! exact)
        {
            char tmp[32];
            std::snprintf (tmp, sizeof (tmp), "x%.3f", out.ratio[k]);
            out.label[k] = tmp;
        }
    }
    return out;
}
} // namespace fp
