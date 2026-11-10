#pragma once
#include "Utils.h"
#include <array>
#include <vector>
#include <atomic>

namespace fp
{
//==============================================================================
// Ambisonics the way the IEM plug-ins do it, up to 7th order (64 channels): ACN channel order with
// SN3D normalisation (AmbiX), no Condon-Shortley phase. Coordinates as IEM and SPARTA use them: x to
// the front, y to the left, z up; azimuth turns to the left, elevation goes up.
//
// Every function takes the order it should work to; a channel beyond it is left at 0, and the
// per-sample loops only run over the (order + 1)^2 channels in use, so 3rd order costs what it
// always did. SN3D means the omni channel is 1 and every order l has mean square 1/(2l+1) over the
// sphere (the tests check this, and that the channels are orthogonal, at every order).
namespace ambi
{
constexpr int kMaxOrder = 7;
constexpr int kOrder = 3;     // the default, and the order of the energy map
constexpr int kCh = 64;       // room for 7th order
using Coefs = std::array<float, kCh>;

constexpr int chFor (int order) noexcept { return (order + 1) * (order + 1); }
inline int orderFor (int channels) noexcept   // the highest order that fits in that many channels
{
    int o = 0;
    while (o < kMaxOrder && chFor (o + 1) <= channels) ++o;
    return o;
}

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

inline int orderOf (int acn) noexcept { int l = 0; while ((l + 1) * (l + 1) <= acn) ++l; return l; }

// SN3D normalisation, sqrt ((2 - delta_m0) (l - m)! / (l + m)!), for every l and m >= 0
inline const std::array<std::array<double, kMaxOrder + 1>, kMaxOrder + 1>& sn3d()
{
    static const auto t = []
    {
        std::array<std::array<double, kMaxOrder + 1>, kMaxOrder + 1> n {};
        auto fact = [] (int k) { double f = 1.0; for (int i = 2; i <= k; ++i) f *= i; return f; };
        for (int l = 0; l <= kMaxOrder; ++l)
            for (int m = 0; m <= l; ++m)
                n[(size_t) l][(size_t) m] = std::sqrt ((m == 0 ? 1.0 : 2.0) * fact (l - m) / fact (l + m));
        return n;
    }();
    return t;
}

// SN3D real spherical harmonics of a unit direction, ACN order, up to 'order'. Worked out in
// Cartesian form: (x + iy)^m gives cos(m az) and sin(m az) times cos(el)^m, and the associated
// Legendre functions (without the Condon-Shortley phase) divided by cos(el)^m are polynomials in z,
// built up by the usual recurrence.
inline Coefs sh (Vec3 d, int order = kMaxOrder) noexcept
{
    order = std::clamp (order, 0, kMaxOrder);
    Coefs y {};
    const double x = d.x, yy = d.y, z = d.z;
    const auto& N = sn3d();
    double cm = 1.0, sm = 0.0;   // Re and Im of (x + iy)^m
    double qmm = 1.0;            // Q_m^m = (2m - 1)!!
    for (int m = 0; m <= order; ++m)
    {
        if (m > 0)
        {
            const double c = cm * x - sm * yy, s = cm * yy + sm * x;
            cm = c; sm = s;
            qmm *= (2.0 * m - 1.0);
        }
        double q2 = 0.0, q1 = qmm;   // Q_{l-2}^m, Q_{l-1}^m
        for (int l = m; l <= order; ++l)
        {
            double q;
            if (l == m) q = qmm;
            else if (l == m + 1) q = z * (2.0 * m + 1.0) * qmm;
            else q = ((2.0 * l - 1.0) * z * q1 - (l + m - 1.0) * q2) / (double) (l - m);
            if (l > m) { q2 = q1; q1 = q; }
            const double nq = N[(size_t) l][(size_t) m] * q;
            y[(size_t) (l * l + l + m)] = (float) (nq * cm);
            if (m > 0) y[(size_t) (l * l + l - m)] = (float) (nq * sm);
        }
    }
    return y;
}

inline Vec3 fromAzEl (float azRad, float elRad) noexcept
{
    return { std::cos (elRad) * std::cos (azRad), std::cos (elRad) * std::sin (azRad), std::sin (elRad) };
}

// max-rE weights for an order N (Zotter and Frank): P_l(cos(137.9 deg / (N + 1.51))), l = 0..N
inline const std::array<float, kMaxOrder + 1>& maxRE (int order = kOrder)
{
    static const auto table = []
    {
        std::array<std::array<float, kMaxOrder + 1>, kMaxOrder + 1> t {};
        for (int N = 0; N <= kMaxOrder; ++N)
        {
            const double c = std::cos (137.9 * kPi / 180.0 / (N + 1.51));
            double p0 = 1.0, p1 = c;
            for (int l = 0; l <= N; ++l)
            {
                double p = l == 0 ? 1.0 : l == 1 ? c : ((2.0 * l - 1.0) * c * p1 - (l - 1.0) * p0) / l;
                if (l >= 2) { p0 = p1; p1 = p; }
                t[(size_t) N][(size_t) l] = (float) p;
            }
        }
        return t;
    }();
    return table[(size_t) std::clamp (order, 0, kMaxOrder)];
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
inline std::vector<Coefs> samplingDecoder (const std::vector<Vec3>& dirs, int order = kOrder)
{
    const auto& w = maxRE (order);
    double e = 0.0;
    for (int l = 0; l <= order; ++l) e += (2 * l + 1) * (double) w[(size_t) l] * w[(size_t) l];
    const float K = (float) dirs.size();
    const float g = (float) std::sqrt (K / e) / K;
    std::vector<Coefs> D (dirs.size());
    for (size_t k = 0; k < dirs.size(); ++k)
    {
        const auto y = sh (dirs[k], order);
        D[k] = {};
        for (int n = 0; n < chFor (order); ++n)
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
inline Coefs encodeInside (Vec3 p, int order = kOrder) noexcept
{
    const float r = std::min (1.0f, p.length());
    const auto y = sh (p.normalised(), order);
    const float g = 1.0f / (0.5f + 0.5f * std::min (1.0f, r / 0.7f));   // full level from 0.7 out
    const float near = std::min (1.0f, r / 0.35f);
    Coefs c {};
    float wl = 1.0f;
    for (int l = 0; l <= order; ++l, wl *= near)
        for (int n = l * l; n < chFor (l); ++n) c[(size_t) n] = g * wl * y[(size_t) n];
    return c;
}

// the direction only (no level change): the higher orders fade as p nears the middle, as in
// encodeInside; the distance model sets the level
inline Coefs encodeNear (Vec3 p, int order = kOrder) noexcept
{
    const float r = std::min (1.0f, p.length());
    const auto y = sh (p.normalised(), order);
    const float near = std::min (1.0f, r / 0.12f);   // (inside about a metre and a half of a 12 m sphere)
    Coefs c {};
    float wl = 1.0f;
    for (int l = 0; l <= order; ++l, wl *= near)
        for (int n = l * l; n < chFor (l); ++n) c[(size_t) n] = wl * y[(size_t) n];
    return c;
}

// a stereo pair at p: left and right turned apart round the vertical by +-halfAngle
inline void encodeStereo (Vec3 p, float halfAngle, Coefs& left, Coefs& right, int order = kOrder) noexcept
{
    const float r = std::min (1.0f, p.length());
    const Vec3 d = r > 1.0e-6f ? p * (1.0f / std::max (1.0e-6f, p.length())) : Vec3 { 1, 0, 0 };
    auto turn = [&] (float a)
    {
        const float c = std::cos (a), s = std::sin (a);
        return Vec3 { d.x * c - d.y * s, d.x * s + d.y * c, d.z } * std::max (r, 1.0e-4f);
    };
    left  = encodeInside (turn ( halfAngle), order);
    right = encodeInside (turn (-halfAngle), order);
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
// delay network run straight on the ambisonic channels (16 at 3rd order, all 64 at 7th). Channel k
// feeds line k and line k comes back out on channel k; the other lines take part only through the mixing, an orthonormal
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
        // (at higher orders the channels share the same 64 lines, so the tail's level per channel is kept the same)
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
    void setChannels (int n) noexcept { nch = std::clamp (n, 1, kCh); }

    // in: the send; out: the reverb (overwritten); both the channels in use (setChannels)
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
                if (k < nch) out[k][i] = y * orderWeight[k];
                v[k] = (y + (k < nch ? in[k][i] : 0.0f)) * g[k];
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
    int nch = 16;
};

//==============================================================================
// The energy meter (after the IEM EnergyVisualizer): rather than decoding to hundreds of
// directions on the audio thread, it keeps the 16 x 16 covariance of the scene (136 numbers),
// smoothed over about 100 ms. The editor asks for the level in any direction: y' C y with
// max-rE weighted harmonics, which is what a decoder pointing there would hear.
class EnergyMeter
{
public:
    static constexpr int kMeterCh = 16;   // (3rd order: the map looks the same at any order, and costs a 16th of 7th order's)
    static constexpr int kPairs = kMeterCh * (kMeterCh + 1) / 2;

    void prepare (double sampleRate) { sr = sampleRate; for (auto& v : acc) v = 0.0; for (auto& c : cov) c.store (0.0f); }

    void process (const float* const* in, int n) noexcept
    {
        if (n <= 0) return;
        for (auto& v : acc) v = 0.0;
        for (int i = 0; i < n; i += 2)   // every second sample is plenty for a meter
        {
            float x[kMeterCh];
            for (int c = 0; c < kMeterCh; ++c) x[c] = in[c][i];
            int p = 0;
            for (int a = 0; a < kMeterCh; ++a)
                for (int b = a; b < kMeterCh; ++b) acc[(size_t) p++] += (double) (x[a] * x[b]);
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
        const auto y = sh (d.normalised(), 3);
        const auto& w = maxRE (3);
        float v[kMeterCh];
        for (int c = 0; c < kMeterCh; ++c) v[c] = y[(size_t) c] * w[(size_t) orderOf (c)] * (float) (2 * orderOf (c) + 1) / 16.0f;
        float e = 0.0f;
        int p = 0;
        for (int a = 0; a < kMeterCh; ++a)
            for (int b = a; b < kMeterCh; ++b)
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
