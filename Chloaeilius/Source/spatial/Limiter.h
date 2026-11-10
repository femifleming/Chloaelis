#pragma once
#include "Utils.h"
#include <vector>

namespace fp
{
//==============================================================================
// The output stage, the last thing on the main output, in two parts. Below -12 dBFS
// both leave the signal bit-for-bit untouched.
//
// Leveler: a gentle stereo-linked compressor (soft knee from -12 dBFS, 4:1, 4 ms attack,
// 250 ms release). It rides the long swells, such as the feedback presets singing on their
// own, so the limiter rarely has to work.
//
// PeakLimiter: a look-ahead brickwall at -1 dBFS. The audio is delayed by 1.5 ms while the
// gain looks ahead, holds the deepest reduction needed over that window and glides into it
// (a moving average), so a peak is turned down smoothly before it arrives instead of being
// chopped. That glide is what keeps it free of distortion. Recovery is 150 ms.
class Leveler
{
public:
    void prepare (double sr)
    {
        atk = (float) (1.0 - std::exp (-1.0 / (0.004 * sr)));
        rel = (float) (1.0 - std::exp (-1.0 / (0.250 * sr)));
        env = 0.0f; gain = 1.0f;
    }

    void process (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float pk = std::max (std::abs (L[i]), std::abs (R[i]));
            env += (pk > env ? atk : rel) * (pk - env);
            env = flushDenormal (env);
            float target = 1.0f;
            if (env > kneeLo)
            {
                const float db = 20.0f * std::log10 (env);
                float over;   // dB above the threshold, through a soft knee
                if (db < threshDb + knee * 0.5f)
                {
                    const float x = db - (threshDb - knee * 0.5f);
                    over = x * x / (2.0f * knee);
                }
                else over = db - threshDb;
                target = std::pow (10.0f, -over * (1.0f - 1.0f / ratio) / 20.0f);
            }
            gain = target;
            if (gain < 1.0f) { L[i] *= gain; R[i] *= gain; }
        }
    }

    float getGain() const noexcept { return gain; }

private:
    static constexpr float threshDb = -9.0f, knee = 6.0f, ratio = 4.0f;
    const float kneeLo = std::pow (10.0f, (threshDb - knee * 0.5f) / 20.0f);
    float atk = 0.01f, rel = 0.0001f, env = 0.0f, gain = 1.0f;
};

class PeakLimiter
{
public:
    static constexpr float ceiling = 0.891f;   // -1 dBFS: headroom for converters and inter-sample peaks

    void prepare (double sr)
    {
        look = std::max (8, (int) (0.0015 * sr));
        release = (float) (1.0 - std::exp (-1.0 / (0.150 * sr)));
        for (auto* b : { &dL, &dR, &need, &held }) b->assign ((size_t) look, b == &need || b == &held ? 1.0f : 0.0f);
        pos = 0; relGain = 1.0f; relRed = 0.0f; sum = (double) look; gain = 1.0f; recount = 0;
    }

    int latency() const noexcept { return look - 1; }

    void process (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            // what this incoming sample needs, and the least over the look-ahead window
            const float pk = std::max (std::abs (L[i]), std::abs (R[i]));
            need[(size_t) pos] = pk > ceiling ? ceiling / pk : 1.0f;
            float hold = 1.0f;
            for (int k = 0; k < look; ++k) hold = std::min (hold, need[(size_t) k]);
            // release: down at once, back up slowly. (It tracks the reduction, 1 - gain: stepping the
            // gain itself up towards 1 stalls just short of it once each step is smaller than a float
            // can resolve near 1, and would leave the output 0.002 dB down for good)
            const float wantRed = 1.0f - hold;
            if (wantRed > relRed) relRed = wantRed;
            else { relRed += release * (wantRed - relRed); if (relRed < 1.0e-6f) relRed = std::max (0.0f, wantRed); }
            relGain = 1.0f - relRed;
            // glide: the average of the last 'look' values (never above what the delayed sample needs)
            sum += (double) relGain - (double) held[(size_t) pos];
            held[(size_t) pos] = relGain;
            if (++recount >= 4096) { recount = 0; sum = 0.0; for (float h : held) sum += (double) h; }   // no drift
            gain = (float) (sum / (double) look);
            if (gain > 0.999999f) gain = 1.0f;

            // out comes the sample from 'look - 1' samples ago
            const int out = (pos + 1) % look;
            const float l = dL[(size_t) out], r = dR[(size_t) out];
            dL[(size_t) pos] = L[i];
            dR[(size_t) pos] = R[i];
            pos = out;
            L[i] = gain < 1.0f ? std::clamp (l * gain, -ceiling, ceiling) : std::clamp (l, -ceiling, ceiling);   // (the clamp never bites)
            R[i] = gain < 1.0f ? std::clamp (r * gain, -ceiling, ceiling) : std::clamp (r, -ceiling, ceiling);
        }
    }

    float getGain() const noexcept { return gain; }

private:
    std::vector<float> dL, dR, need, held;
    int look = 72, pos = 0, recount = 0;
    float release = 0.0005f, relGain = 1.0f, relRed = 0.0f, gain = 1.0f;
    double sum = 72.0;
};
//==============================================================================
// The same two, linked across any number of channels (the 16 of the sphere, or the two of the
// stereo and binaural outputs): one gain for all of them, so the scene never tilts.
class MultiLeveler
{
public:
    void prepare (double sr) { atk = (float) (1.0 - std::exp (-1.0 / (0.004 * sr))); rel = (float) (1.0 - std::exp (-1.0 / (0.250 * sr))); env = 0.0f; gain = 1.0f; }
    void process (float* const* ch, int numCh, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            float pk = 0.0f;
            for (int c = 0; c < numCh; ++c) pk = std::max (pk, std::abs (ch[c][i]));
            env += (pk > env ? atk : rel) * (pk - env);
            env = flushDenormal (env);
            float target = 1.0f;
            if (env > kneeLo)
            {
                const float db = 20.0f * std::log10 (env);
                float over;
                if (db < threshDb + knee * 0.5f) { const float x = db - (threshDb - knee * 0.5f); over = x * x / (2.0f * knee); }
                else over = db - threshDb;
                target = std::pow (10.0f, -over * (1.0f - 1.0f / ratio) / 20.0f);
            }
            gain = target;
            if (gain < 1.0f) for (int c = 0; c < numCh; ++c) ch[c][i] *= gain;
        }
    }
    float getGain() const noexcept { return gain; }
private:
    static constexpr float threshDb = -3.0f, knee = 6.0f, ratio = 4.0f;
    const float kneeLo = std::pow (10.0f, (threshDb - knee * 0.5f) / 20.0f);
    float atk = 0.01f, rel = 0.0001f, env = 0.0f, gain = 1.0f;
};

class MultiLimiter
{
public:
    static constexpr float ceiling = PeakLimiter::ceiling;
    static constexpr int kMaxCh = 64;

    void prepare (double sr)
    {
        look = std::max (8, (int) (0.0015 * sr));
        release = (float) (1.0 - std::exp (-1.0 / (0.150 * sr)));
        for (auto& d : dl) d.assign ((size_t) look, 0.0f);
        need.assign ((size_t) look, 1.0f); held.assign ((size_t) look, 1.0f);
        pos = 0; relRed = 0.0f; sum = (double) look; gain = 1.0f; recount = 0;
    }
    int latency() const noexcept { return look - 1; }

    void process (float* const* ch, int numCh, int n) noexcept
    {
        numCh = std::min (numCh, kMaxCh);
        for (int i = 0; i < n; ++i)
        {
            float pk = 0.0f;
            for (int c = 0; c < numCh; ++c) pk = std::max (pk, std::abs (ch[c][i]));
            need[(size_t) pos] = pk > ceiling ? ceiling / pk : 1.0f;
            float hold = 1.0f;
            for (int k = 0; k < look; ++k) hold = std::min (hold, need[(size_t) k]);
            const float wantRed = 1.0f - hold;
            if (wantRed > relRed) relRed = wantRed;
            else { relRed += release * (wantRed - relRed); if (relRed < 1.0e-6f) relRed = std::max (0.0f, wantRed); }
            const float relGain = 1.0f - relRed;
            sum += (double) relGain - (double) held[(size_t) pos];
            held[(size_t) pos] = relGain;
            if (++recount >= 4096) { recount = 0; sum = 0.0; for (float h : held) sum += (double) h; }
            gain = (float) (sum / (double) look);
            if (gain > 0.999999f) gain = 1.0f;
            const int out = (pos + 1) % look;
            for (int c = 0; c < numCh; ++c)
            {
                const float x = dl[c][(size_t) out];
                dl[c][(size_t) pos] = ch[c][i];
                ch[c][i] = std::clamp (gain < 1.0f ? x * gain : x, -ceiling, ceiling);
            }
            pos = out;
        }
    }
    float getGain() const noexcept { return gain; }
private:
    std::vector<float> dl[kMaxCh], need, held;
    int look = 72, pos = 0, recount = 0;
    float release = 0.0005f, relRed = 0.0f, gain = 1.0f;
    double sum = 72.0;
};
} // namespace fp
