#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <array>
#include <atomic>

namespace fp
{
constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

//==============================================================================
// 4-point, 3rd-order Hermite interpolation (low aliasing, cheap)
inline float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// smooth "S" curve 0..1 -> 0..1 with zero slope at the ends (click-free slews)
inline float smoothstep (float x) noexcept
{
    x = std::clamp (x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// a safety saturator for feedback loops and resonators: exactly linear up to 0.7, then bends
// smoothly towards 1 (same slope at the knee, so no corner). Ordinary levels pass untouched;
// only a runaway loop gets caught.
inline float softClip (float x) noexcept
{
    constexpr float k = 0.7f;
    const float a = std::abs (x);
    if (a <= k) return x;
    const float y = k + (1.0f - k) * std::tanh ((a - k) / (1.0f - k));
    return x < 0.0f ? -y : y;
}

// Grain envelope shapes (t = 0..1 through the grain). All start and end at zero, so none click.
//   0 saw (the original: slow rise, quick smooth drop)   1 reverse saw   2 bell   3 triangle   4 pulse
//==============================================================================
// A grain envelope you draw on the chalkboard: 64 heights across the grain, read with
// linear interpolation. Written by the editor, read by the audio thread (relaxed atomics:
// a half-updated drawing for one block is harmless).
struct ChalkShape
{
    static constexpr int N = 64;
    std::array<std::atomic<float>, N> v;
    std::atomic<float> power { 0.5f };   // mean square, for level matching

    ChalkShape() { for (int i = 0; i < N; ++i) v[(size_t) i].store (std::sin (3.14159265f * (float) i / (float) (N - 1))); updatePower(); }

    float at (float t) const noexcept
    {
        const float x = std::clamp (t, 0.0f, 1.0f) * (float) (N - 1);
        const int i = std::min ((int) x, N - 2);
        const float f = x - (float) i;
        const float a = v[(size_t) i].load (std::memory_order_relaxed), b = v[(size_t) i + 1].load (std::memory_order_relaxed);
        return a + f * (b - a);
    }
    void set (int i, float h) noexcept { v[(size_t) std::clamp (i, 0, N - 1)].store (std::clamp (h, 0.0f, 1.0f), std::memory_order_relaxed); }
    void updatePower() noexcept
    {
        double s = 0.0;
        for (int k = 0; k < 256; ++k) { const float e = at ((float) k / 255.0f); s += e * e; }
        power.store (std::max (0.02f, (float) (s / 256.0)));
    }
};

// grain envelopes: 0 saw, 1 reverse saw, 2 bell, 3 triangle, 4 pulse, 5 sine, 6 chalk (drawn)
inline float grainShape (int shape, float t, const ChalkShape* chalk = nullptr) noexcept
{
    t = std::clamp (t, 0.0f, 1.0f);
    auto saw = [] (float u) { return u < 0.85f ? u / 0.85f : 0.5f * (1.0f + std::cos (3.14159265f * (u - 0.85f) / 0.15f)); };
    switch (shape)
    {
        case 1:  return saw (1.0f - t);
        case 2:  return 0.5f - 0.5f * std::cos (6.2831853f * t);
        case 3:  return 1.0f - std::abs (2.0f * t - 1.0f);
        case 5:  return std::sin (3.14159265f * t);
        case 6:  return chalk != nullptr ? chalk->at (t) : std::sin (3.14159265f * t);
        case 4:  { auto ss = [] (float x) { x = std::clamp (x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); };
                   return ss (t / 0.12f) * ss ((1.0f - t) / 0.12f); }
        default: return saw (t);
    }
}
// mean square of each shape, to keep the grain level steady when the shape or density changes
inline float grainShapePower (int shape, const ChalkShape* chalk = nullptr) noexcept
{
    static const float p[7] = { 0.34f, 0.34f, 0.375f, 0.333f, 0.80f, 0.5f, 0.5f };
    if (shape == 6 && chalk != nullptr) return chalk->power.load (std::memory_order_relaxed);
    return p[std::clamp (shape, 0, 6)];
}

inline float flushDenormal (float x) noexcept
{
    return (std::abs (x) < 1.0e-20f) ? 0.0f : x;
}

//==============================================================================
// small, fast, real-time safe random generator (xorshift64*)
struct Rng
{
    uint64_t s = 0x9E3779B97F4A7C15ull;

    void seed (uint64_t v) noexcept { s = v ? v : 0x9E3779B97F4A7C15ull; }

    uint64_t next() noexcept
    {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ull;
    }

    float uni() noexcept { return (float) ((next() >> 40) * (1.0 / 16777216.0)); } // [0,1)
    float range (float lo, float hi) noexcept { return lo + (hi - lo) * uni(); }
    float bi() noexcept { return uni() * 2.0f - 1.0f; }
    int   integer (int n) noexcept { return (int) (uni() * (float) n) % n; }
};

//==============================================================================
// Transposed direct form II biquad (RBJ cookbook)
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() noexcept { z1 = z2 = 0; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        b0 = (float) (nb0 / na0); b1 = (float) (nb1 / na0); b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0); a2 = (float) (na2 / na0);
    }

    void setLowpass (double sr, double f, double q) noexcept
    {
        const double w = kTwoPi * std::min (f, sr * 0.49) / sr, c = std::cos (w), a = std::sin (w) / (2.0 * q);
        set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a);
    }

    void setHighpass (double sr, double f, double q) noexcept
    {
        const double w = kTwoPi * std::min (f, sr * 0.49) / sr, c = std::cos (w), a = std::sin (w) / (2.0 * q);
        set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + a, -2 * c, 1 - a);
    }
};

// 8th-order Butterworth lowpass (4 cascaded biquads) - used as anti-alias filter
struct Butter8LP
{
    Biquad s[4];
    void setup (double sr, double f)
    {
        static const double qs[4] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
        for (int i = 0; i < 4; ++i) s[i].setLowpass (sr, f, qs[i]);
    }
    void reset() { for (auto& b : s) b.reset(); }
    float process (float x) noexcept
    {
        for (auto& b : s) x = b.process (x);
        return x;
    }
};

// one-pole DC blocker
struct DCBlocker
{
    float r = 0.9995f, x1 = 0, y1 = 0;
    void setup (double sr, double hz = 10.0) { r = (float) std::exp (-kTwoPi * hz / sr); }
    void reset() { x1 = y1 = 0; }
    float process (float x) noexcept
    {
        const float y = x - x1 + r * y1;
        x1 = x; y1 = flushDenormal (y);
        return y1;
    }
};

// one-pole lowpass
struct OnePoleLP
{
    float a = 1.0f, z = 0.0f;
    void setup (double sr, double hz) { a = (float) (1.0 - std::exp (-kTwoPi * hz / sr)); }
    void reset() { z = 0; }
    float process (float x) noexcept { z += a * (x - z); z = flushDenormal (z); return z; }
};

//==============================================================================
// Linear parameter ramp (per-sample), used for every user-facing control so
// knob moves are always click-free.
struct Ramp
{
    float cur = 0, target = 0, step = 0;
    int   left = 0, len = 1;

    void setup (double sr, double seconds) { len = std::max (1, (int) (sr * seconds)); }
    void snap (float v) { cur = target = v; left = 0; step = 0; }
    void set (float v)
    {
        if (v == target) return;
        target = v; left = len; step = (target - cur) / (float) len;
    }
    float next() noexcept
    {
        if (left > 0) { cur += step; if (--left == 0) cur = target; }
        return cur;
    }
};

//==============================================================================
// A value in 0..1 that wanders to random targets at random times, gliding with a
// random slew time and an S-curve so the motion never has corners.
struct RandomWalk
{
    float value = 0.5f, from = 0.5f, to = 0.5f, phase = 1.0f, inc = 0.0f;
    int   hold = 0;
    double sr = 48000.0;
    float minHold = 0.15f, maxHold = 2.5f, minSlew = 0.03f, maxSlew = 2.0f;
    Rng*  rng = nullptr;

    void prepare (double sampleRate, Rng& r, float minH, float maxH, float minS, float maxS)
    {
        sr = sampleRate; rng = &r; minHold = minH; maxHold = maxH; minSlew = minS; maxSlew = maxS;
        value = from = to = rng->uni(); phase = 1.0f; hold = (int) (sr * rng->range (minHold, maxHold));
    }

    float next() noexcept
    {
        if (phase < 1.0f)
        {
            phase = std::min (1.0f, phase + inc);
            value = from + (to - from) * smoothstep (phase);
        }
        else if (--hold <= 0)
        {
            from  = value;
            to    = rng->uni();
            // random slew, skewed so short and long glides both happen
            const float u = rng->uni();
            const float slew = minSlew * std::pow (maxSlew / minSlew, u);
            inc   = 1.0f / (float) std::max (1.0, slew * sr);
            phase = 0.0f;
            hold  = (int) (sr * (slew + rng->range (minHold, maxHold)));
        }
        return value;
    }
};

//==============================================================================
// Fractional circular delay line (power-of-two size) with Hermite reads.
struct DelayLine
{
    std::vector<float> buf;
    int mask = 0, w = 0;

    void prepare (int minSize)
    {
        int n = 1;
        while (n < minSize + 4) n <<= 1;
        buf.assign ((size_t) n, 0.0f);
        mask = n - 1; w = 0;
    }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }

    void push (float x) noexcept { buf[(size_t) w] = x; w = (w + 1) & mask; }

    // delay in samples, measured from the most recently pushed sample (must be >= 1)
    float read (float delay) const noexcept
    {
        const float p  = (float) (w - 1) - delay;
        const float fl = std::floor (p);
        const int   i  = (int) fl;
        const float t  = p - fl;
        return hermite (buf[(size_t) ((i - 1) & mask)], buf[(size_t) (i & mask)],
                        buf[(size_t) ((i + 1) & mask)], buf[(size_t) ((i + 2) & mask)], t);
    }

    float readInt (int delay) const noexcept { return buf[(size_t) ((w - 1 - delay) & mask)]; }
};
} // namespace fp
