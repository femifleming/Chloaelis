#pragma once
#include "Utils.h"
#include <array>
#include <vector>
#include <atomic>

namespace fp
{
//==============================================================================
// Third-order Ambisonics, the way the IEM plug-ins do it: 16 channels in ACN order with SN3D
// normalisation (AmbiX). Coordinates as IEM and SPARTA use them: x to the front, y to the left,
// z up; azimuth turns to the left, elevation goes up.
//
// Real spherical harmonics up to order 3, written out in Cartesian form for a unit vector. SN3D
// means the omni channel is 1 and every order l has mean square 1/(2l+1) over the sphere (the
// offline test checks this, and that the 16 are orthogonal).
namespace ambi
{
constexpr int kOrder = 3;
constexpr int kCh = 16;
using Coefs = std::array<float, kCh>;

struct Vec3
{
    float x = 0, y = 0, z = 0;
    Vec3 operator+ (Vec3 o) const noexcept { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator- (Vec3 o) const noexcept { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator* (float k) const noexcept { return { x * k, y * k, z * k }; }
    Vec3& operator+= (Vec3 o) noexcept { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-= (Vec3 o) noexcept { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*= (float k) noexcept { x *= k; y *= k; z *= k; return *this; }
    float dot (Vec3 o) const noexcept { return x * o.x + y * o.y + z * o.z; }
    float length() const noexcept { return std::sqrt (x * x + y * y + z * z); }
    Vec3 normalised() const noexcept { const float l = length(); return l > 1.0e-9f ? *this * (1.0f / l) : Vec3 { 1, 0, 0 }; }
};

inline int orderOf (int acn) noexcept { return acn < 1 ? 0 : acn < 4 ? 1 : acn < 9 ? 2 : 3; }

// SN3D real spherical harmonics of a unit direction, ACN order
inline Coefs sh (Vec3 d) noexcept
{
    const float x = d.x, y = d.y, z = d.z;
    constexpr float s3 = 1.7320508f, s15 = 3.8729833f, s58 = 0.7905694f, s38 = 0.6123724f;
    return { 1.0f,
             y, z, x,
             s3 * x * y, s3 * y * z, 0.5f * (3.0f * z * z - 1.0f), s3 * x * z, 0.5f * s3 * (x * x - y * y),
             s58 * y * (3.0f * x * x - y * y), s15 * x * y * z, s38 * y * (5.0f * z * z - 1.0f), 0.5f * z * (5.0f * z * z - 3.0f),
             s38 * x * (5.0f * z * z - 1.0f), 0.5f * s15 * z * (x * x - y * y), s58 * x * (x * x - 3.0f * y * y) };
}

inline Vec3 fromAzEl (float azRad, float elRad) noexcept
{
    return { std::cos (elRad) * std::cos (azRad), std::cos (elRad) * std::sin (azRad), std::sin (elRad) };
}

// max-rE weights for order 3 (Zotter and Frank): P_l(cos(137.9 deg / (N + 1.51)))
inline const std::array<float, kOrder + 1>& maxRE()
{
    static const std::array<float, kOrder + 1> w = []
    {
        const double c = std::cos (137.9 * kPi / 180.0 / (kOrder + 1.51));
        return std::array<float, kOrder + 1> { 1.0f, (float) c, (float) (0.5 * (3 * c * c - 1)), (float) (0.5 * (5 * c * c * c - 3 * c)) };
    }();
    return w;
}

// points spread evenly over the sphere (a Fibonacci lattice); 'mirrored' adds each point's
// reflection left to right, so a layout made from them treats both ears exactly alike
inline std::vector<Vec3> fibonacci (int n, bool mirrored = false)
{
    if (mirrored)
    {
        auto p = fibonacci (n);
        const size_t half = p.size();
        for (size_t i = 0; i < half; ++i) p.push_back ({ p[i].x, -p[i].y, p[i].z });
        return p;
    }
    std::vector<Vec3> p ((size_t) n);
    const double golden = kPi * (3.0 - std::sqrt (5.0));
    for (int i = 0; i < n; ++i)
    {
        const double z = 1.0 - 2.0 * (i + 0.5) / n, r = std::sqrt (std::max (0.0, 1.0 - z * z)), a = golden * i;
        p[(size_t) i] = { (float) (r * std::cos (a)), (float) (r * std::sin (a)), (float) z };
    }
    return p;
}

// A sampling decoder to virtual directions with max-rE weighting, scaled so that a plane wave
// from anywhere puts unit energy across them.
inline std::vector<Coefs> samplingDecoder (const std::vector<Vec3>& dirs)
{
    const auto& w = maxRE();
    double e = 0.0;
    for (int l = 0; l <= kOrder; ++l) e += (2 * l + 1) * (double) w[(size_t) l] * w[(size_t) l];
    const float K = (float) dirs.size();
    const float g = (float) std::sqrt (K / e) / K;
    std::vector<Coefs> D (dirs.size());
    for (size_t k = 0; k < dirs.size(); ++k)
    {
        const auto y = sh (dirs[k]);
        for (int n = 0; n < kCh; ++n)
        {
            const int l = orderOf (n);
            D[k][(size_t) n] = g * (float) (2 * l + 1) * w[(size_t) l] * y[(size_t) n];
        }
    }
    return D;
}

//==============================================================================
// Where a balloon (a grain) sits inside the sphere becomes its 16 gains. At the surface (r = 1)
// it's a plain plane-wave encoding; nearer the middle the higher orders fade (r below 0.35 starts
// to spread it, and at the very centre it's all omni: inside your head) and inside 0.7 it gets
// louder, up to +6 dB at the centre.
inline Coefs encodeInside (Vec3 p) noexcept
{
    const float r = std::min (1.0f, p.length());
    const auto y = sh (p.normalised());
    const float g = 1.0f / (0.5f + 0.5f * std::min (1.0f, r / 0.7f));   // full level from 0.7 out
    const float near = std::min (1.0f, r / 0.35f);
    const float wl[4] = { 1.0f, near, near * near, near * near * near };
    Coefs c;
    for (int n = 0; n < kCh; ++n) c[(size_t) n] = g * wl[orderOf (n)] * y[(size_t) n];
    return c;
}

// the direction only (no level change): the higher orders fade as p nears the middle, as in
// encodeInside; the distance model sets the level
inline Coefs encodeNear (Vec3 p) noexcept
{
    const float r = std::min (1.0f, p.length());
    const auto y = sh (p.normalised());
    const float near = std::min (1.0f, r / 0.12f);   // (inside about a metre and a half of a 12 m sphere)
    const float wl[4] = { 1.0f, near, near * near, near * near * near };
    Coefs c;
    for (int n = 0; n < kCh; ++n) c[(size_t) n] = wl[orderOf (n)] * y[(size_t) n];
    return c;
}

// a stereo pair at p: left and right turned apart round the vertical by +-halfAngle
inline void encodeStereo (Vec3 p, float halfAngle, Coefs& left, Coefs& right) noexcept
{
    const float r = std::min (1.0f, p.length());
    const Vec3 d = r > 1.0e-6f ? p * (1.0f / std::max (1.0e-6f, p.length())) : Vec3 { 1, 0, 0 };
    auto turn = [&] (float a)
    {
        const float c = std::cos (a), s = std::sin (a);
        return Vec3 { d.x * c - d.y * s, d.x * s + d.y * c, d.z } * std::max (r, 1.0e-4f);
    };
    left  = encodeInside (turn ( halfAngle));
    right = encodeInside (turn (-halfAngle));
}

//==============================================================================
// A decorrelator: three Schroeder allpasses in a row (flat in level, scrambled in phase), with
// delays picked per copy and per channel from a table of primes between 1.5 and 9 ms. Two copies of
// the same effect played from two places would comb-filter once decoded (the same signal arriving
// from two directions); through different decorrelators they add up like two sounds instead.
class Decorrelator
{
public:
    void prepare (double sr, int which)
    {
        static const int ms100[] = { 151, 173, 197, 211, 239, 263, 281, 307, 331, 359, 383, 409, 433, 461, 487, 509,
                                     541, 563, 593, 617, 643, 673, 701, 733, 761, 787, 811, 839, 863, 887 };
        constexpr int n = (int) (sizeof (ms100) / sizeof (ms100[0]));
        for (int k = 0; k < 3; ++k)
        {
            const int pick = (which * 7 + k * 11 + (which / 3) * 5) % n;
            len[k] = std::max (2, (int) (ms100[pick] * 0.01 * 0.001 * sr * (1.0 + 0.37 * k)));
            line[k].assign ((size_t) len[k], 0.0f);
            pos[k] = 0;
        }
    }
    void reset() { for (int k = 0; k < 3; ++k) { std::fill (line[k].begin(), line[k].end(), 0.0f); pos[k] = 0; } }
    void process (float* x, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            float v = x[i];
            for (int k = 0; k < 3; ++k)
            {
                const float d = line[k][(size_t) pos[k]];
                const float w = v + 0.5f * d;
                line[k][(size_t) pos[k]] = flushDenormal (w);
                if (++pos[k] >= len[k]) pos[k] = 0;
                v = d - 0.5f * w;
            }
            x[i] = v;
        }
    }
private:
    std::vector<float> line[3];
    int len[3] {}, pos[3] {};
};

//==============================================================================
// The one reverb for the whole sphere, built the way the IEM FdnReverb is: a 64-line feedback
// delay network run straight on the 16 ambisonic channels. Channel k feeds line k and line k comes
// back out on channel k; the other 48 lines take part only through the mixing, an orthonormal
// Hadamard matrix (fast Walsh-Hadamard transform, divided by 8). The delays come from the same
// recipe as IEM's: entries of a table of primes picked by a growing step (room size 20), each
// prime / 10000 seconds, so from under a millisecond to about half a second, none sharing a
// factor. Every line loses 60 dB over the decay time (gain 10^(-3 d / T60) for a line d seconds
// long), and two shelves in each line set how much faster or slower the highs and lows go:
// IEM's defaults, -10 dB/s above 2 kHz and +1 dB/s below 100 Hz.
// The output is the reverb alone (no direct sound); the caller mixes it in.
class SphereReverb
{
public:
    static constexpr int L = 64;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        // odd primes from 3, as IEM's table
        std::vector<int> primes;
        for (int k = 3; (int) primes.size() < 1200; k += 2)
        {
            bool prime = true;
            for (int d = 3; d * d <= k; d += 2) if (k % d == 0) { prime = false; break; }
            if (prime) primes.push_back (k);
        }
        const int room = 20, first = room / 10, last = room;
        int idx = std::max (first, 1);
        for (int k = 0; k < L; ++k)
        {
            if (k > 0)
            {
                const float inc = std::max (1.0f, (float) first + std::abs ((float) (last - first)) / (float) L * (float) k);
                idx = (int) std::lround ((float) idx + inc);
            }
            const double seconds = primes[(size_t) std::min (idx, (int) primes.size() - 1)] / 10000.0;
            len[k] = std::max (2, (int) (seconds * sr));
            lines[k].assign ((size_t) len[k], 0.0f);
            pos[k] = 0;
        }
        // an even, diffuse field in SN3D has each order l at 1 / (2l + 1) of the omni channel's energy;
        // the network gives every channel the same, so the higher orders are brought down to that
        for (int c = 0; c < kCh; ++c) orderWeight[c] = 1.0f / std::sqrt ((float) (2 * orderOf (c) + 1));   // (W as it was)
        decayNow = -1.0f;
        setDecay (3.0f);
        update();
        reset();
    }

    void reset()
    {
        for (int k = 0; k < L; ++k) { std::fill (lines[k].begin(), lines[k].end(), 0.0f); pos[k] = 0; lo[k].reset(); hi[k].reset(); }
    }

    void setDecay (float seconds) noexcept { decayTarget = std::clamp (seconds, 0.1f, 30.0f); }

    // in: the send (16 channels); out: the reverb (16 channels, overwritten)
    void process (const float* const* in, float* const* out, int n) noexcept
    {
        if (decayNow != decayTarget)
        {
            // glide the decay (a jump would step every line's gain at once)
            decayNow = decayNow < 0.0f ? decayTarget : decayNow + (decayTarget - decayNow) * std::min (1.0f, (float) n / (float) (0.05 * sr));
            if (std::abs (decayNow - decayTarget) < 1.0e-3f) decayNow = decayTarget;
            update();
        }
        float v[L];
        for (int i = 0; i < n; ++i)
        {
            for (int k = 0; k < L; ++k)
            {
                float y = lines[k][(size_t) pos[k]];
                y = lo[k].process (hi[k].process (y));
                if (k < kCh) out[k][i] = y * orderWeight[k];
                v[k] = (y + (k < kCh ? in[k][i] : 0.0f)) * g[k];
            }
            fwht (v);
            for (int k = 0; k < L; ++k)
            {
                lines[k][(size_t) pos[k]] = flushDenormal (v[k]);
                if (++pos[k] >= len[k]) pos[k] = 0;
            }
        }
    }

private:
    static void fwht (float* x) noexcept
    {
        for (int h = 1; h < L; h <<= 1)
            for (int i = 0; i < L; i += h << 1)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = x[j], b = x[j + h];
                    x[j] = a + b; x[j + h] = a - b;
                }
        for (int i = 0; i < L; ++i) x[i] *= 0.125f;   // 1 / sqrt(64)
    }

    // a shelf (RBJ cookbook), 'gain' the linear gain of the shelved band
    static void shelf (Biquad& f, double sr, double hz, double q, double gain, bool low)
    {
        const double A = std::sqrt (std::max (1.0e-6, gain)), w = kTwoPi * hz / sr, cw = std::cos (w), al = std::sin (w) / (2.0 * q), sa = 2.0 * std::sqrt (A) * al;
        if (low) f.set (A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa),
                         (A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa);
        else     f.set (A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa),
                         (A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa);
    }

    void update() noexcept
    {
        const double overall = std::pow (10.0, -3.0 / (double) decayNow);   // per second
        const double lowPerSec = std::pow (10.0, 1.0 / 20.0), highPerSec = std::pow (10.0, -10.0 / 20.0);
        for (int k = 0; k < L; ++k)
        {
            const double d = len[k] / sr;
            g[k] = (float) std::pow (overall, d);
            shelf (lo[k], sr, 100.0, 0.5, std::pow (lowPerSec, d), true);
            shelf (hi[k], sr, 2000.0, 0.5, std::pow (highPerSec, d), false);
        }
    }

    double sr = 48000.0;
    std::vector<float> lines[L];
    int len[L] {}, pos[L] {};
    float g[L] {};
    Biquad lo[L], hi[L];
    float decayNow = -1.0f, decayTarget = 3.0f;
    float orderWeight[kCh] {};
};

//==============================================================================
// The energy meter (after the IEM EnergyVisualizer): rather than decoding to hundreds of
// directions on the audio thread, it keeps the 16 x 16 covariance of the scene (136 numbers),
// smoothed over about 100 ms. The editor asks for the level in any direction: y' C y with
// max-rE weighted harmonics, which is what a decoder pointing there would hear.
class EnergyMeter
{
public:
    static constexpr int kPairs = kCh * (kCh + 1) / 2;

    void prepare (double sampleRate) { sr = sampleRate; for (auto& v : acc) v = 0.0; for (auto& c : cov) c.store (0.0f); }

    void process (const float* const* in, int n) noexcept
    {
        if (n <= 0) return;
        for (auto& v : acc) v = 0.0;
        for (int i = 0; i < n; i += 2)   // every second sample is plenty for a meter
        {
            float x[kCh];
            for (int c = 0; c < kCh; ++c) x[c] = in[c][i];
            int p = 0;
            for (int a = 0; a < kCh; ++a)
                for (int b = a; b < kCh; ++b) acc[(size_t) p++] += (double) (x[a] * x[b]);
        }
        const double count = (double) ((n + 1) / 2);
        const float k = (float) (1.0 - std::exp (-(double) n / (0.1 * sr)));
        for (int p = 0; p < kPairs; ++p)
        {
            const float now = (float) (acc[(size_t) p] / count);
            const float old = cov[(size_t) p].load (std::memory_order_relaxed);
            cov[(size_t) p].store (flushDenormal (old + k * (now - old)), std::memory_order_relaxed);
        }
    }

    // mean square heard from direction d
    float levelAt (Vec3 d) const noexcept
    {
        const auto y = sh (d.normalised());
        const auto& w = maxRE();
        float v[kCh];
        for (int c = 0; c < kCh; ++c) v[c] = y[(size_t) c] * w[(size_t) orderOf (c)] * (float) (2 * orderOf (c) + 1) / 16.0f;
        float e = 0.0f;
        int p = 0;
        for (int a = 0; a < kCh; ++a)
            for (int b = a; b < kCh; ++b)
            {
                const float c = cov[(size_t) p++].load (std::memory_order_relaxed);
                e += (a == b ? 1.0f : 2.0f) * v[a] * v[b] * c;
            }
        return std::max (0.0f, e);
    }

    std::array<std::atomic<float>, kPairs> cov {};
private:
    double sr = 48000.0;
    std::array<double, kPairs> acc {};
};
} // namespace ambi
} // namespace fp
