#pragma once
#include "Ambi.h"
#include <array>
#include <vector>

namespace fp
{
//==============================================================================
// How far away something is, after the IEM RoomEncoder: the sound reaches you along its direct
// path and off the six walls of a room round the sphere (first-order image sources), each path
// with its own
//   * delay: its length / 343 m/s, read with Hermite interpolation and glided across each block,
//     so a thing flying towards you or away bends in pitch (Doppler) as it would,
//   * level: 1 / distance (as RoomEncoder), unity at 4 m: inside a small bubble everything is near
//     and loud, inside a big one far and quiet (1.5 m: about +8 dB, 24 m: about -14 dB at the edge),
//     never more than +12 dB, and each wall takes another 9 dB off,
//   * colour: air takes a little of the highs off with distance (a one-pole lowpass falling from
//     20 kHz near you to about 16 kHz at 6 m and 9 kHz at 30 m), and each wall takes some more (RoomEncoder's shelves do this per
//     reflection order),
//   * direction: the direct path through ambi::encodeNear (so inside a metre or so it spreads round
//     your head), the reflections as plane waves from their image sources.
// Positions come in as fractions of the bubble's radius (|p| <= 1); radiusM says how many metres
// that is (the Distance knob: the size of the bubble you're in). The room's walls stand just outside the sphere; the listener is at its centre.
class DistanceSource
{
public:
    static constexpr int kTaps = 7;   // direct + 6 walls
    static inline bool withWalls = true;   // (a test turns the walls off to check the direct path alone)
    static constexpr float refMetres = 4.0f;   // at 4 m a sound is at unity; nearer louder (up to +12 dB), further quieter (1 / distance)

    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        size = 1;
        while (size < (int) (0.45 * sr) + maxBlock + 8) size <<= 1;   // up to ~150 m of path
        ring.assign ((size_t) size, 0.0f);
        tmp.assign ((size_t) std::max (1, maxBlock), 0.0f);
        w = 0; fresh = true;
        for (auto& t : taps) t = Tap {};
    }

    void reset() { std::fill (ring.begin(), ring.end(), 0.0f); w = 0; fresh = true; for (auto& t : taps) t = Tap {}; }

    // mono in; adds into 'out' (16 channels) and, for the direct path, 'send' (the reverb) at sendGain
    void process (const float* in, int n, ambi::Vec3 posTo, float radiusM, float* const* out, float* const* send, float sendGain) noexcept
    {
        if ((int) tmp.size() < n) tmp.resize ((size_t) n);
        for (int i = 0; i < n; ++i) { ring[(size_t) w] = in[i]; w = (w + 1) & (size - 1); }

        const ambi::Vec3 s = ambi::Vec3 { posTo.x, posTo.y, posTo.z } * radiusM;   // metres
        const float H = radiusM * 1.6f + 2.0f;                                        // the walls, well clear of the sphere
        const ambi::Vec3 img[kTaps] = { s,
                                        {  2 * H - s.x, s.y, s.z }, { -2 * H - s.x, s.y, s.z },
                                        { s.x,  2 * H - s.y, s.z }, { s.x, -2 * H - s.y, s.z },
                                        { s.x, s.y,  2 * H - s.z }, { s.x, s.y, -2 * H - s.z } };
        const float ref = refMetres;   // a fixed reference: the same sound is louder in a small bubble than a big one
        const float inv = 1.0f / (float) n;
        for (int k = 0; k < (withWalls ? kTaps : 1); ++k)
        {
            auto& t = taps[(size_t) k];
            const float d = std::max (k == 0 ? 0.3f : 0.5f, img[k].length());
            const float delay = d / 343.0f * (float) sr;
            const float gain = std::min (4.0f, ref / d) * (k == 0 ? 1.0f : 0.35f);   // each wall -9 dB: enough to say how far, not enough to blur where
            const float air = 20000.0f / (1.0f + d / 25.0f) * (k == 0 ? 1.0f : 0.55f);
            const float a = 1.0f - std::exp (-(float) kTwoPi * std::min (air, (float) sr * 0.45f) / (float) sr);
            ambi::Coefs c = k == 0 ? ambi::encodeNear (posTo) : ambi::sh (img[k].normalised());
            if (fresh) { t.delay = delay; t.gain = gain; t.coef = c; t.a = a; }

            // the path: read back along the ring (gliding the delay), filter, level
            const float d0 = t.delay, dd = (delay - d0) * inv, g0 = t.gain, dg = (gain - g0) * inv;
            float lp = t.lp;
            for (int i = 0; i < n; ++i)
            {
                const float dl = d0 + dd * (float) (i + 1);
                const float x = read ((float) (w - n + i) - dl);
                lp += a * (x - lp);
                tmp[(size_t) i] = lp * (g0 + dg * (float) (i + 1));
            }
            t.lp = flushDenormal (lp);
            t.delay = delay; t.gain = gain; t.a = a;

            // into the 16 channels, the gains gliding from the last block's direction
            for (int ch = 0; ch < ambi::kCh; ++ch)
            {
                const float c0 = t.coef[(size_t) ch], c1 = c[(size_t) ch];
                if (c0 == 0.0f && c1 == 0.0f) continue;
                const float dc = (c1 - c0) * inv;
                float* o = out[ch];
                float* sd = (k == 0 && send != nullptr) ? send[ch] : nullptr;
                for (int i = 0; i < n; ++i)
                {
                    const float y = (c0 + dc * (float) (i + 1)) * tmp[(size_t) i];
                    o[i] += y;
                    if (sd != nullptr) sd[i] += sendGain * y;
                }
            }
            t.coef = c;
        }
        fresh = false;
    }

private:
    struct Tap { float delay = 0.0f, gain = 0.0f, a = 1.0f, lp = 0.0f; ambi::Coefs coef {}; };

    float read (float pos) const noexcept
    {
        const float f = std::floor (pos);
        const int i = (int) f;
        const float t = pos - f;
        const int m = size - 1;
        return hermite (ring[(size_t) ((i - 1) & m)], ring[(size_t) (i & m)], ring[(size_t) ((i + 1) & m)], ring[(size_t) ((i + 2) & m)], t);
    }

    double sr = 48000.0;
    std::vector<float> ring, tmp;
    int size = 1, w = 0;
    bool fresh = true;
    std::array<Tap, kTaps> taps {};
};
} // namespace fp
