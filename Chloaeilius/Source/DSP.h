#pragma once
// Chloaelis DSP: analog-style multimode SVF, a two-stage 12AX7 tube model (4x oversampled)
// and three delay engines: PT2399 (1-bit adaptive delta modulator with a fixed 44k-bit RAM and a variable clock),
// bucket brigade (companded, clocked charge buckets) and an ageing tape loop. Plus a Lorenz attractor for modulation.
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <vector>

namespace chl::dsp
{
    constexpr float pi    = 3.14159265358979f;
    // per-voice parameter slots (real units); must match chl::ModParam's first entries
    enum VoiceParam { vIn, vCut, vRes, vMorph, vDrive, vTime, vRegen, vWet, vOut };
    constexpr float twoPi = 6.28318530717959f;

    inline float fastTanh (float x)
    {
        if (x > 3.0f)  return 1.0f;
        if (x < -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    inline float onePoleCoef (float fc, float sr) { return 1.0f - std::exp (-twoPi * fc / sr); }

    //==========================================================================
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

        void lowpass (float fc, float sr, float q = 0.7071f)
        {
            fc = juce::jlimit (10.0f, sr * 0.45f, fc);
            const float w = twoPi * fc / sr, c = std::cos (w), al = std::sin (w) / (2.0f * q), n = 1.0f / (1.0f + al);
            b0 = (1.0f - c) * 0.5f * n; b1 = (1.0f - c) * n; b2 = b0; a1 = -2.0f * c * n; a2 = (1.0f - al) * n;
        }
        void peak (float fc, float sr, float q, float db)
        {
            fc = juce::jlimit (10.0f, sr * 0.45f, fc);
            const float A = std::pow (10.0f, db / 40.0f), w = twoPi * fc / sr, c = std::cos (w), al = std::sin (w) / (2.0f * q);
            const float n = 1.0f / (1.0f + al / A);
            b0 = (1.0f + al * A) * n; b1 = -2.0f * c * n; b2 = (1.0f - al * A) * n; a1 = b1; a2 = (1.0f - al / A) * n;
        }
        void highpass (float fc, float sr, float q = 0.7071f)
        {
            fc = juce::jlimit (5.0f, sr * 0.45f, fc);
            const float w = twoPi * fc / sr, c = std::cos (w), al = std::sin (w) / (2.0f * q), n = 1.0f / (1.0f + al);
            b0 = (1.0f + c) * 0.5f * n; b1 = -(1.0f + c) * n; b2 = b0; a1 = -2.0f * c * n; a2 = (1.0f - al) * n;
        }
        inline float process (float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() { z1 = z2 = 0; }
    };

    struct DCBlock
    {
        float r = 0.999f, x1 = 0, y1 = 0;
        void prepare (float sr, float fc = 8.0f) { r = 1.0f - twoPi * fc / sr; reset(); }
        inline float process (float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
        void reset() { x1 = y1 = 0; }
    };

    //==========================================================================
    // Analog-flavoured state variable filter (Zavalishin TPT topology) with a saturating
    // integrator, continuously morphing LP > BP > HP > Notch.
    struct MultiModeFilter
    {
        float sr = 48000.0f, ic1 = 0, ic2 = 0;

        void prepare (float s) { sr = s; reset(); }
        void reset() { ic1 = ic2 = 0; }

        inline float process (float x, float fc, float res, float morph)
        {
            fc = juce::jlimit (12.0f, sr * 0.46f, fc);
            const float g = std::tan (pi * fc / sr);
            const float k = 2.0f - 1.96f * juce::jlimit (0.0f, 1.0f, res);
            x = fastTanh (x * 0.6f) * (1.0f / 0.6f);              // input stage
            const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            ic1 = 3.0f * fastTanh (ic1 * (1.0f / 3.0f));          // OTA-ish soft limit on the band integrator

            const float lp = v2, bp = v1 * k, hp = x - k * v1 - v2, notch = lp + hp;
            const float outs[5] = { lp, bp, hp, notch, notch };
            const float m = juce::jlimit (0.0f, 3.0f, morph);
            const int s = (int) m;
            const float t = m - (float) s;
            return outs[s] + (outs[s + 1] - outs[s]) * t;
        }
    };

    //==========================================================================
    // One 12AX7 common-cathode stage. Plate current follows a softplus^1.5 law (cut-off on
    // the negative swing, smooth compression on the positive), the grid conducts above 0 V
    // and charges the coupling cap, which shifts the bias (blocking distortion / sag).
    struct TriodeStage
    {
        float bias = 0, att = 0.01f, rel = 0.0005f, ip0 = 0, invSlope = 1;

        static inline float softplus (float x) { return x > 12.0f ? x : std::log1p (std::exp (x)); }
        static inline float plate (float vg) { const float s = softplus (4.0f * (vg + 1.0f)) * 0.25f; return s * std::sqrt (s); }

        void prepare (float osr)
        {
            ip0 = plate (0.0f);
            invSlope = 0.002f / (plate (0.001f) - plate (-0.001f));
            att = onePoleCoef (1.0f / (twoPi * 0.002f), osr);
            rel = onePoleCoef (1.0f / (twoPi * 0.120f), osr);
            bias = 0;
        }

        // shift > 0 biases the stage toward cut-off (a starved plate supply): small signals
        // fall into the dead zone and sputter, big ones come through fuzzed and lopsided
        inline float process (float vg, float shift = 0.0f)
        {
            vg -= bias;
            float g = vg - shift;
            if (g > 0.0f) g = g / (1.0f + 0.85f * g);           // grid conduction clamps positive swing
            const float charge = juce::jmax (0.0f, vg) * 0.35f;
            bias += (charge - bias) * (charge > bias ? att : rel);
            return (plate (g) - (shift > 0.0f ? plate (-shift) : ip0)) * invSlope;
        }
    };

    // Tube types: 0 = 12AX7 (two triode stages, warm, even harmonics, blocking distortion; the
    // default), 1 = EL84 (a 12AX7 into a push-pull pentode pair: symmetric, crunchy odd
    // harmonics and a hard pentode knee), 2 = 6L6 (a push-pull beam pair with power-supply sag:
    // tighter at first, then it blooms and compresses as the rail droops), 3 = starved (a 12AX7
    // at a low plate voltage, biased near cut-off: gated, sputtery, lopsided fuzz).
    struct TubeStage
    {
        float osr = 192000.0f, miller = 0, out1 = 0, millerA = 0.3f, postA = 0.5f;
        float sag = 0, sagAtt = 0.01f, sagRel = 0.001f;
        int type = 0;
        TriodeStage v1, v2;
        DCBlock inter, dc;

        void prepare (float oversampledRate)
        {
            osr = oversampledRate;
            v1.prepare (osr); v2.prepare (osr);
            inter.prepare (osr, 12.0f); dc.prepare (osr, 7.0f);
            postA = onePoleCoef (15000.0f, osr);
            sagAtt = onePoleCoef (1.0f / (twoPi * 0.012f), osr);
            sagRel = onePoleCoef (1.0f / (twoPi * 0.25f), osr);
            miller = out1 = sag = 0;
        }

        static inline float pentode (float v, float knee)
        {
            const float a = juce::jmax (0.0f, v + 0.7f);
            return std::tanh (a * a * knee);
        }

        // drive 0..1 (per sample)
        inline float process (float x, float drive)
        {
            const float gain = std::exp2 (drive * 7.0f);          // 0..42 dB first-stage gain
            millerA = onePoleCoef (18000.0f - 10500.0f * drive, osr);   // Miller capacitance gets worse with gain
            float s = x * gain * 0.5f;
            miller += millerA * (s - miller);

            if (type == 0)
            {
                const float c2 = 0.35f + 0.9f * drive;             // second-stage drive
                float y = v1.process (miller);
                y = inter.process (y);                             // coupling cap
                y = v2.process (y * c2);                           // second stage, phase kept so even harmonics stack
                out1 += postA * (y - out1);
                const float makeup = 1.0f / (0.5f * c2 * std::pow (gain, 0.36f));
                return dc.process (out1) * makeup;
            }
            if (type == 3)   // starved
            {
                const float shift = 0.55f - 0.25f * drive;
                float y = v1.process (miller * 1.6f, shift);
                y = inter.process (y);
                y = v2.process (y * (0.6f + 0.9f * drive), shift * 0.6f);
                out1 += postA * (y - out1);
                const float makeup = 2.2f / (std::pow (gain, 0.42f) * (0.6f + 0.9f * drive))
                                     * std::exp2 ((1.2f + 2.0f * drive * drive) * 0.16609640474f);   // level-matched to the 12AX7
                return dc.process (out1) * makeup;
            }
            float y = v1.process (miller);
            y = inter.process (y);
            const float c2 = 0.5f + 1.6f * drive;
            float v = y * c2, p;
            if (type == 1)   // EL84 pair
                p = 0.5f * (pentode (v, 0.9f) - pentode (-v, 0.9f));
            else             // 6L6 pair with sag: the rail droops with the current drawn
            {
                const float rail = 1.0f / (1.0f + 2.2f * sag);
                v *= 0.8f * rail;
                p = 0.5f * (pentode (v, 0.55f) - pentode (-v, 0.55f)) * (0.75f + 0.25f * rail);
                const float draw = std::abs (p);
                sag += (draw - sag) * (draw > sag ? sagAtt : sagRel);
            }
            out1 += postA * (p - out1);
            const float matchDb = type == 1 ? 4.0f * drive + 6.0f * drive * drive : 9.0f * drive - 2.5f * drive * drive;
            const float makeup = std::exp2 (matchDb * 0.16609640474f) / ((type == 1 ? 0.53f : 0.27f) * c2 * std::pow (gain, 0.4f));
            return dc.process (out1) * makeup;
        }
    };

    // The oversampled tube. Changing the type copies the running stage's state into a second
    // stage set to the new type, then crossfades to it (40 ms), so a switch never drops out or
    // clicks.
    struct Tube
    {
        juce::dsp::Oversampling<float> os { 1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
        TubeStage stages[2];
        int cur = 0;
        bool xfading = false;
        float xf = 0, xfStep = 0.0001f, osr = 192000.0f;
        float glow = 0, glowA = 0.001f, onMix = 1.0f, onStep = 0.001f;

        void prepare (double sr, int maxBlock)
        {
            os.initProcessing ((size_t) maxBlock);
            osr = (float) sr * 4.0f;
            for (auto& st : stages) st.prepare (osr);
            glowA = onePoleCoef (6.0f, osr);
            onStep = 1.0f / (0.006f * osr);
            xfStep = 1.0f / (0.04f * osr);
            reset();
        }
        void reset() { os.reset(); glow = 0; xfading = false; for (auto& st : stages) { const int t = st.type; st.prepare (osr); st.type = t; } }
        float latency() const { return os.getLatencyInSamples(); }

        // the oversampler always runs (constant latency); "on" crossfades the triodes in/out
        void process (float* data, int n, float driveStart, float driveEnd, bool on = true, int wantType = 0)
        {
            if (! xfading && wantType != stages[cur].type)
            {
                // the new stage starts as a copy of the running one (same first triode, coupling
                // cap and bias state), then switches type: nothing has to settle from cold
                auto& nx = stages[1 - cur];
                nx = stages[cur];
                nx.type = wantType;
                nx.sag = 0.0f;
                xfading = true; xf = -0.25f;     // 10 ms unheard, then the crossfade
            }
            float* chans[1] = { data };
            juce::dsp::AudioBlock<float> block (chans, 1, (size_t) n);
            auto up = os.processSamplesUp (block);
            float* u = up.getChannelPointer (0);
            const int un = (int) up.getNumSamples();
            const float step = (driveEnd - driveStart) / (float) juce::jmax (1, un);
            float d = driveStart;
            const float target = on ? 1.0f : 0.0f;
            for (int i = 0; i < un; ++i, d += step)
            {
                onMix += juce::jlimit (-onStep, onStep, target - onMix);
                const float x = u[i];
                float y = x;
                if (onMix > 0.0f)
                {
                    y = stages[cur].process (x, d);
                    if (xfading)
                    {
                        const float yb = stages[1 - cur].process (x, d);
                        const float c = juce::jlimit (0.0f, 1.0f, xf);
                        const float k = c * c * (3.0f - 2.0f * c);          // smooth s-curve
                        y += (yb - y) * k;
                        xf += xfStep;
                        if (xf >= 1.0f) { cur = 1 - cur; xfading = false; }
                    }
                }
                u[i] = x + (y - x) * onMix;
                glow += glowA * (std::abs (y) * (0.25f + d) * onMix - glow);
            }
            os.processSamplesDown (block);
        }
    };

    //==========================================================================
    // The delay engines below are bare "lines" (input -> coloured, delayed output). The
    // regeneration loop, its HPF and the slew / fade logic live in DelayCell, the way the
    // feedback lives in the op-amps around the chip on the real module.
    //==========================================================================

    //==========================================================================
    // PT2399. The chip is a continuously variable slope delta modulator writing 1 bit per
    // conversion into a fixed 44 kbit RAM. The delay time is set by the VCO clock: longer
    // delays mean a slower bit rate, so more quantisation noise, slope overload grit,
    // narrower bandwidth and aliasing (the output is just the integrated bit stream sampled
    // and held). Turning time while it runs re-clocks the RAM, so the echoes pitch-bend.
    // Freeze stops writing the RAM, so the loop recirculates forever, bit for bit.
    struct PT2399Line
    {
        static constexpr int kBits = 44000;
        static constexpr float kClockPerBit = 15.55f;   // ~684k VCO cycles for the full RAM at 342 ms

        std::vector<uint8_t> mem = std::vector<uint8_t> (kBits, 0);
        int pos = 0;
        float sr = 48000.0f, bitRate = 130000.0f, targetRate = 130000.0f, bitPhase = 0, prevIn = 0, held = 0;
        float encEst = 0, encStep = 0, decEst = 0, decStep = 0;
        unsigned encHist = 0, decHist = 0;
        float leak = 0.999f, beta = 0.99f, dMin = 0.001f, dMax = 0.1f;
        float whinePhase = 0, whineInc = 0, clockA = 0.01f;
        Biquad inLP, outLP1, outLP2;
        DCBlock dc;
        juce::Random rng { 0x2399 };

        void prepare (float s)
        {
            sr = s;
            clockA = onePoleCoef (12.0f, sr);       // the VCO's RC smoothing on the time pot
            dc.prepare (sr, 10.0f);
            reset();
        }

        void reset()
        {
            for (size_t i = 0; i < mem.size(); ++i) mem[i] = (uint8_t) (i & 1);   // idle pattern = silence
            pos = 0; bitPhase = 0; prevIn = held = 0;
            encEst = decEst = 0; encStep = decStep = 0; encHist = decHist = 0;
            whinePhase = 0;
            inLP.reset(); outLP1.reset(); outLP2.reset(); dc.reset();
            bitRate = targetRate = kBits / 0.33f;
            updateCoeffs();
        }

        // jump = re-clock instantly (used by the fade mode, which waits for the RAM to refill)
        void setTime (float sec, bool jump)
        {
            targetRate = (float) kBits / juce::jlimit (0.02f, 1.6f, sec);
            if (jump) { bitRate = targetRate; updateCoeffs(); }
        }

        // called once per control block
        void updateCoeffs()
        {
            // the chip's RC integrator / syllabic filter time constants are fixed in time,
            // so per-bit coefficients scale with the clock
            leak = std::exp (-twoPi * 25.0f / bitRate);
            beta = std::exp (-1.0f / (0.0035f * bitRate));
            // step size ~ 1/sqrt(clock): long delays overload sooner (grit), and re-clocking a
            // loop changes its level the way the chip does, without +30 dB bursts
            const float isq = 1.0f / std::sqrt (bitRate);
            dMin = 2.6f * isq;
            dMax = 145.0f * isq;
            const float bw = juce::jlimit (2400.0f, 11500.0f, bitRate * 0.048f);
            inLP.lowpass (bw, sr, 0.62f);
            outLP1.lowpass (bw, sr, 0.54f);
            outLP2.lowpass (bw * 1.25f, sr, 1.3f);
            const float clk = bitRate * kClockPerBit;
            float f = std::fmod (clk, sr);
            if (f > sr * 0.5f) f = sr - f;
            whineInc = f / sr;
        }

        inline float process (float x, bool freeze)
        {
            bitRate += (targetRate - bitRate) * clockA;

            float in = freeze ? 0.0f : x;
            in = inLP.process (in);
            in = 1.1f * fastTanh (in * 0.9f);          // the chip's input opamp rails

            bitPhase += bitRate / sr;
            const int n = (int) bitPhase;
            bitPhase -= (float) n;

            if (n > 0)
            {
                float acc = 0;
                const float invN = 1.0f / (float) n;
                for (int k = 1; k <= n; ++k)
                {
                    const float xin = prevIn + (in - prevIn) * ((float) k * invN);

                    // encoder (CVSD, run-of-3 coincidence)
                    const unsigned bit = xin > encEst ? 1u : 0u;
                    encHist = ((encHist << 1) | bit) & 7u;
                    encStep = beta * encStep + (1.0f - beta) * ((encHist == 0u || encHist == 7u) ? dMax : dMin);
                    encEst = encEst * leak + (bit ? encStep : -encStep);

                    // RAM: read the oldest bit, then (unless frozen) overwrite it
                    const unsigned outBit = mem[(size_t) pos];
                    if (! freeze) mem[(size_t) pos] = (uint8_t) bit;
                    if (++pos >= kBits) pos = 0;

                    // decoder
                    decHist = ((decHist << 1) | outBit) & 7u;
                    decStep = beta * decStep + (1.0f - beta) * ((decHist == 0u || decHist == 7u) ? dMax : dMin);
                    decEst = decEst * leak + (outBit ? decStep : -decStep);
                    acc += decEst;
                }
                held = acc * invN;                     // sample & hold of the bit-rate output (aliases)
            }
            prevIn = in;

            float y = outLP2.process (outLP1.process (held));
            y = 1.3f * fastTanh (dc.process (y) * (1.0f / 1.3f));       // output opamp rails

            // noise floor + a faint clock whine folded into the audio band
            whinePhase += whineInc; if (whinePhase >= 1.0f) whinePhase -= 1.0f;
            y += (rng.nextFloat() - 0.5f) * 0.00012f + std::sin (twoPi * whinePhase) * 0.00018f;
            return y;
        }
    };

    //==========================================================================
    // Bucket brigade (think MN3005 pairs, as in the old analog delays and the Serge
    // BBD module): NE570-style 2:1 compressor > fixed anti-alias filter > a chain of
    // charge buckets clocked at kStages / time > sample-and-hold output with clock
    // feedthrough > fixed reconstruction filter > 1:2 expander. The filters are fixed while
    // the clock moves, so long delays alias and go crunchy and dark; buckets clip softly and
    // leak a little each transfer; the compander breathes and pumps the noise floor.
    struct BBDLine
    {
        static constexpr int kStages = 4096;        // buckets per output (two 4096-stage chips, one output tap)
        std::vector<float> bucket = std::vector<float> (kStages, 0.0f);
        int idx = 0;
        float sr = 48000.0f, clk = 12000.0f, targetClk = 12000.0f, clkA = 0.001f, tickPhase = 0;
        float prevAA = 0, held = 0, cti = 0, parity = 1.0f;
        float compEnv = 0.05f, expEnv = 0.05f, cAtt = 0.01f, cRel = 0.001f, eAtt = 0.01f, eRel = 0.001f;
        Biquad aa1, aa2, rc1, rc2;
        DCBlock dc;
        juce::Random rng { 0xbbd };
        static constexpr float ref = 0.22f;

        void prepare (float s)
        {
            sr = s;
            clkA = onePoleCoef (9.0f, sr);
            aa1.lowpass (6800.0f, sr, 0.54f); aa2.lowpass (6800.0f, sr, 1.31f);
            rc1.lowpass (6200.0f, sr, 0.54f); rc2.lowpass (6200.0f, sr, 1.31f);
            cAtt = onePoleCoef (1.0f / (twoPi * 0.004f), sr); cRel = onePoleCoef (1.0f / (twoPi * 0.045f), sr);
            eAtt = onePoleCoef (1.0f / (twoPi * 0.006f), sr); eRel = onePoleCoef (1.0f / (twoPi * 0.060f), sr);
            dc.prepare (sr, 9.0f);
            reset();
        }
        void reset()
        {
            std::fill (bucket.begin(), bucket.end(), 0.0f);
            idx = 0; tickPhase = 0; prevAA = held = cti = 0; compEnv = expEnv = 0.05f;
            aa1.reset(); aa2.reset(); rc1.reset(); rc2.reset(); dc.reset();
            clk = targetClk = (float) kStages / 0.33f;
        }
        void setTime (float sec, bool jump)
        {
            targetClk = (float) kStages / juce::jlimit (0.02f, 1.6f, sec);
            if (jump) clk = targetClk;
        }
        void updateCoeffs() {}

        static inline float bucketSat (float v)
        {
            // charge wells fill up softly, a little asymmetrically
            return 0.85f * (fastTanh (v * 1.15f + 0.06f) - 0.0599f);
        }

        inline float process (float x, bool freeze)
        {
            clk += (targetClk - clk) * clkA;

            // compressor (2:1)
            const float ax = std::abs (x);
            compEnv += (ax - compEnv) * (ax > compEnv ? cAtt : cRel);
            const float g = juce::jmin (10.0f, 1.0f / std::sqrt (juce::jmax (compEnv, 0.0005f) / ref));
            const float aa = aa2.process (aa1.process (x * g));

            const float inc = clk / sr;
            tickPhase += inc;
            while (tickPhase >= 1.0f)
            {
                tickPhase -= 1.0f;
                // where inside this sample the clock edge landed
                const float frac = juce::jlimit (0.0f, 1.0f, 1.0f - tickPhase / inc);
                const float v = prevAA + (aa - prevAA) * frac;
                const float out = bucket[(size_t) idx];
                bucket[(size_t) idx] = freeze ? out * 0.99985f : bucketSat (v) + (rng.nextFloat() - 0.5f) * 0.0009f;
                if (++idx >= kStages) idx = 0;
                cti += 0.86f * (out - cti);                        // charge transfer inefficiency
                held = cti;
                parity = -parity;
            }
            prevAA = aa;

            // S&H output + a whisper of clock feedthrough, then the reconstruction filter
            float y = rc2.process (rc1.process (held + parity * 0.0012f));
            y += (rng.nextFloat() - 0.5f) * 0.0004f;               // output noise, expanded with the signal

            // expander (1:2)
            const float ay = std::abs (y);
            expEnv += (ay - expEnv) * (ay > expEnv ? eAtt : eRel);
            y *= juce::jmin (6.0f, juce::jmax (expEnv, 0.0002f) / ref);
            y = dc.process (y);
            return 1.25f * fastTanh (y * (1.0f / 1.25f));
        }
    };

    //==========================================================================
    // Tape. Record head (pre-emphasis, soft magnetic saturation) > a loop of oxide that
    // wears > playback head (gap loss, head bump, de-emphasis) with wow and flutter, hiss.
    // The tape ages while it runs: flakes of oxide shed (dropouts that come round again
    // every pass), the gap loss creeps down, the warble widens, the hiss rises, and with
    // freeze the held loop slowly disintegrates pass after pass. A refresh puts on new tape.
    struct TapeLine
    {
        static constexpr int kBlock = 512;
        std::vector<float> buf, dmg;
        int size = 1, w = 0, nb = 1;
        float sr = 48000.0f, age = 0;
        float dA = 0, dS = 0, dB = 0, dTarget = 0, slewA = 0.001f, xf = 1, xfStep = 0.001f;
        bool fading = false, frozen = false;
        float loopStart = 0, loopLen = 1, loopPos = 0;
        float wowT = 0, wow1 = 0, wow2 = 0, wowA = 0.0001f, wowTimer = 0, flutPh = 0, flutPh2 = 0, flutInc = 0, flutInc2 = 0, flutAmp = 1;
        float wowDepth = 100, flutDepth = 1;
        float pre = 0, preA = 0.2f, gap1 = 0, gap2 = 0, deemph = 0, hissLP = 0, hissHP = 0, hissA = 0.5f, hissHA = 0.1f, crackle = 0;
        // the hiss (and crackle) never cuts in or out: it fades up when this tape starts running and
        // down when the engine is switched away (hissTarget, set by the voice), so a recording has no step
        float hissGain = 0, hissTarget = 1, hissUp = 0.00002f, hissDown = 0.00002f;
        Biquad bump;
        DCBlock dc;
        juce::Random rng;

        void prepare (float s, float maxSec, juce::int64 seed)
        {
            sr = s;
            rng.setSeed (seed);
            size = (int) (maxSec * sr) + 16;
            buf.assign ((size_t) size, 0.0f);
            nb = size / kBlock + 1;
            dmg.assign ((size_t) nb, 0.0f);
            slewA = onePoleCoef (1.6f, sr);                         // the capstan servo's inertia
            xfStep = 1.0f / (0.07f * sr);
            wowA = onePoleCoef (0.55f, sr);
            flutInc = (6.1f + rng.nextFloat() * 0.8f) / sr;
            flutInc2 = (11.3f + rng.nextFloat() * 1.5f) / sr;
            wowDepth = 0.0021f * sr;                                // ~0.5% pitch wander when new
            flutDepth = 0.000028f * sr;
            preA = onePoleCoef (2400.0f, sr);
            hissA = onePoleCoef (11000.0f, sr);
            hissHA = onePoleCoef (900.0f, sr);
            hissUp = 1.0f / (1.6f * sr);
            hissDown = 1.0f / (1.4f * sr);
            bump.peak (95.0f, sr, 0.9f, 2.5f);
            dc.prepare (sr, 8.0f);
            reset();
        }
        void reset()
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            w = 0; dA = dS = dB = dTarget = 0.33f * sr; xf = 1; fading = frozen = false;
            pre = gap1 = gap2 = deemph = hissLP = hissHP = crackle = 0;
            hissGain = 0;
            bump.reset(); dc.reset();
        }
        void setTime (float sec, bool jump)
        {
            dTarget = juce::jlimit (64.0f, (float) size - wowDepth * 4.0f - 32.0f, sec * sr);
            if (jump) dA = dS = dB = dTarget;
        }
        void refresh() { std::fill (dmg.begin(), dmg.end(), 0.0f); }

        void addFlake()
        {
            const int b = rng.nextInt (nb);
            const int len = 1 + rng.nextInt (1 + (int) (5.0f * age));
            const float s = (0.15f + 0.75f * rng.nextFloat()) * std::sqrt (age);
            for (int k = 0; k < len; ++k)
            {
                const float wnd = std::sin (pi * ((float) k + 0.5f) / (float) len);
                float& d = dmg[(size_t) ((b + k) % nb)];
                d = juce::jmin (1.0f, juce::jmax (d, s * wnd) + 0.08f * s * wnd);
            }
        }
        // after loading an old session: a tape of this age already has its scars
        void seedDamage() { refresh(); for (int i = 0; i < (int) std::round (25.0f * age * age * age); ++i) addFlake(); }

        // once per control block: oxide sheds as the tape runs
        void wear (float dt)
        {
            const float rate = 0.01f + 0.12f * age * age;            // flakes per second: ~25 over a tape's life, about half the loop scarred
            if (age > 0.001f && rng.nextFloat() < rate * dt) addFlake();
        }

        inline float dmgAt (float pos) const
        {
            const float p = pos / (float) kBlock;
            int i = (int) p;
            const float f = p - (float) i;
            i = ((i % nb) + nb) % nb;
            return dmg[(size_t) i] + (dmg[(size_t) ((i + 1) % nb)] - dmg[(size_t) i]) * f;
        }
        inline float wrapPos (float p) const
        {
            while (p < 0.0f) p += (float) size;
            while (p >= (float) size) p -= (float) size;
            return p;
        }
        inline float readAt (float p) const
        {
            p = wrapPos (p);
            const int i = (int) p;
            const float f = p - (float) i;
            auto at = [this] (int k) { k %= size; if (k < 0) k += size; return buf[(size_t) k]; };
            const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
            const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * f + c2) * f + c1) * f + x0;
        }

        inline float process (float in, bool freeze, bool fadeMode)
        {
            const float wear = std::sqrt (age);

            // wow: a slow random wander; flutter: the capstan and pinch roller
            if (--wowTimer <= 0.0f) { wowT = rng.nextFloat() * 2.0f - 1.0f; wowTimer = sr * (0.5f + 1.8f * rng.nextFloat()); }
            wow1 += (wowT - wow1) * wowA;
            wow2 += (wow1 - wow2) * wowA;
            flutPh += flutInc;   if (flutPh >= 1.0f) flutPh -= 1.0f;
            flutPh2 += flutInc2; if (flutPh2 >= 1.0f) flutPh2 -= 1.0f;
            flutAmp += ((0.5f + rng.nextFloat()) - flutAmp) * 0.0004f;
            float wob = wowDepth * (1.0f + 1.6f * wear) * wow2
                      + flutDepth * (1.0f + 2.0f * wear) * flutAmp * (std::sin (twoPi * flutPh) + 0.35f * std::sin (twoPi * flutPh2));

            if (freeze && ! frozen) { frozen = true; loopLen = juce::jmax (256.0f, dA); loopStart = wrapPos ((float) w - 1.0f - dA); loopPos = 0; }
            else if (! freeze && frozen) frozen = false;

            float out, readPos;
            if (! frozen)
            {
                // record head
                pre += (in - pre) * preA;
                const float e = in + 0.6f * (in - pre);                       // pre-emphasis
                const float m = fastTanh (e * 1.15f + 0.04f) - 0.04f;          // magnetic saturation (a little asymmetric)
                buf[(size_t) w] = m * (1.0f - 0.8f * dmgAt ((float) w)) * (1.0f - 0.18f * wear);
                if (++w >= size) w = 0;

                // playback head(s)
                const float d0 = dA;
                if (! fadeMode)
                {
                    fading = false;
                    dS += (dTarget - dS) * slewA;                              // tape speed glides (pitch bends)
                    dA += (dS - dA) * slewA;
                    out = readAt ((float) w - 1.0f - juce::jmax (8.0f, dA + wob));
                }
                else
                {
                    if (! fading && std::abs (dTarget - dA) > 0.01f * dA + 4.0f) { dB = dTarget; fading = true; xf = 0; }
                    const float ya = readAt ((float) w - 1.0f - juce::jmax (8.0f, dA + wob));
                    if (fading)
                    {
                        const float yb = readAt ((float) w - 1.0f - juce::jmax (8.0f, dB + wob));
                        xf += xfStep;
                        if (xf >= 1.0f) { xf = 1.0f; dA = dB; fading = false; out = yb; }
                        else out = ya * std::cos (xf * pi * 0.5f) + yb * std::sin (xf * pi * 0.5f);
                    }
                    else out = ya;
                    dS = dA;
                }
                readPos = (float) w - 1.0f - d0;
            }
            else
            {
                loopPos += 1.0f;
                if (loopPos >= loopLen) loopPos -= loopLen;
                const float p = loopStart + loopPos - wob * 0.5f;
                out = readAt (p);
                const float X = juce::jmin (600.0f, loopLen * 0.25f);       // hide the splice
                if (loopPos > loopLen - X)
                {
                    const float t = (loopPos - (loopLen - X)) / X;
                    out = out * (1.0f - t) + readAt (p - loopLen) * t;
                }
                // the held loop disintegrates a little each pass, worst where the oxide is gone
                const int ip = (int) wrapPos (loopStart + loopPos);
                const int pp = ip > 0 ? ip - 1 : size - 1;
                const float d = dmg[(size_t) ((ip / kBlock) % nb)];
                float& s = buf[(size_t) ip];
                s += (buf[(size_t) pp] - s) * (0.03f * wear + 0.3f * d);
                s *= 1.0f - (0.0009f * wear + 0.012f * d);
                readPos = p;
            }

            // playback colouring
            const float dr = dmgAt (wrapPos (readPos));
            const float fc = 15000.0f * (1.0f - 0.8f * wear) * (1.0f - 0.55f * dr);
            const float wc = twoPi * fc / sr, a = wc / (1.0f + wc);
            gap1 += (out - gap1) * a;
            gap2 += (gap1 - gap2) * a;
            float y = bump.process (gap2);
            deemph += (y - deemph) * preA;
            y -= 0.375f * (y - deemph);                                   // de-emphasis
            y *= 1.0f - 0.4f * dr;                                        // dropout where oxide shed

            // hiss (recorded with everything else, so it builds with the regen)
            const float nz = rng.nextFloat() * 2.0f - 1.0f;
            hissLP += (nz - hissLP) * hissA;
            hissHP += (hissLP - hissHP) * hissHA;
            hissGain += juce::jlimit (-hissDown, hissUp, hissTarget - hissGain);
            const float hg = hissGain * hissGain * (3.0f - 2.0f * hissGain);
            y += (hissLP - hissHP) * (0.0018f + 0.008f * wear) * hg;

            // crumbling: the odd crackle of a worn splice
            if (wear > 0.25f && rng.nextFloat() < 0.00003f * wear * wear * (1.0f + 8.0f * dr))
                crackle = (rng.nextFloat() * 2.0f - 1.0f) * 0.02f * wear;
            crackle *= 0.92f;
            y += crackle * hg;
            return dc.process (y);
        }
    };

    //==========================================================================
    // Karplus-Strong: a short, clean, exactly tuned line. Inside the delay's regeneration loop it
    // becomes a plucked / bowed string at the pitch whose period is the delay. The string's damping
    // is the classic two-point average, blended in by the delay engine (PT2399 bright, tape softer,
    // BBD dark): its delay is the same at every frequency, so the partials stay harmonic. The line
    // is shortened by the loop's other delays (and lengthened by the DC blocker's phase lead at the
    // fundamental), so the pitch is true. Slew glides between notes; fade crossfades to the new note
    // with no glide. Freeze holds the string.
    struct KSLine
    {
        std::vector<float> buf;
        int size = 1, w = 0;
        float sr = 48000.0f, dA = 100, dB = 100, dTarget = 100, slewA = 0.001f, xf = 1, xfStep = 0.001f, lp = 0;
        float dcLead = 0.0f;
        static constexpr float damp[3] { 0.5f, 0.25f, 1.0f };     // tape, PT2399, BBD: how much of the two-point average
        bool fading = false;
        DCBlock dc;

        void prepare (float s)
        {
            sr = s;
            size = (int) (0.09f * sr) + 16;
            buf.assign ((size_t) size, 0.0f);
            slewA = onePoleCoef (11.0f, sr);
            xfStep = 1.0f / (0.03f * sr);
            dc.prepare (sr, 1.5f);
            dcLead = 1.5f / (twoPi * sr);       // the blocker's phase lead at a note of period P is dcLead * P * P samples
            reset();
        }
        void reset() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; lp = 0; xf = 1; fading = false; dc.reset(); }
        void setPeriod (float sec, bool jump)
        {
            dTarget = juce::jlimit (5.0f, (float) size - 8.0f, sec * sr);
            if (jump) { dA = dB = dTarget; fading = false; xf = 1; }
        }
        inline float readAt (float d) const
        {
            float p = (float) w - d;
            if (p < 0.0f) p += (float) size;
            const int i = (int) p;
            const float f = p - (float) i;
            auto at = [this] (int k) { if (k < 0) k += size; else if (k >= size) k -= size; return buf[(size_t) k]; };
            const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
            const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * f + c2) * f + c1) * f + x0;
        }
        // in = input + the loop's feedback; out = the string. (The newest sample in the line is one old,
        // the feedback arrives a sample later and the damping filter adds its own delay: all taken off.)
        inline float process (float in, bool freeze, bool fadeMode, int type)
        {
            const float d = damp[juce::jlimit (0, 2, type)];
            auto lineFor = [&] (float period) { return juce::jmax (3.0f, freeze ? period : period - 1.0f - 0.5f * d + dcLead * period * period); };
            if (! fadeMode) { fading = false; xf = 1; dA += (dTarget - dA) * slewA; }
            else if (! fading && std::abs (dTarget - dA) > 0.02f) { dB = dTarget; fading = true; xf = 0; }
            float y = readAt (lineFor (dA));
            if (fading)
            {
                const float yb = readAt (lineFor (dB));
                xf += xfStep;
                if (xf >= 1.0f) { xf = 1.0f; dA = dB; fading = false; y = yb; }
                else y = y * std::cos (xf * pi * 0.5f) + yb * std::sin (xf * pi * 0.5f);
            }
            buf[(size_t) w] = freeze ? y : in;      // held: the string goes round on its own, undamped
            if (++w >= size) w = 0;
            if (freeze) return y;
            const float o = y + (lp - y) * (0.5f * d);
            lp = y;
            return dc.process (o);
        }
    };

    //==========================================================================
    // One delay: an engine (tape / PT2399 / BBD) inside a regeneration loop with the optional
    // feedback HPF. Slew mode: time changes glide (the clock or the tape speed moves, so the
    // echoes bend in pitch). Fade mode: the new time is set up on a second line / head and
    // crossfaded in, so nothing changes pitch. For the clocked chips the second line has to
    // fill before it can be heard, which is exactly what a pair of real chips would do.
    struct DelayCell
    {
        enum Type { tape = 0, pt2399 = 1, bbd = 2 };
        PT2399Line pt[2];
        BBDLine bb[2];
        TapeLine tp;
        KSLine ks;
        bool ksMode = false;      // Karplus-Strong: timeSec is then the note's period
        int activeType = pt2399, act = 0, waitN = 0;
        float sr = 48000.0f, typeGain = 1, typeStep = 0.001f, xf = 1, xfStep = 0.001f, lineTime[2] { 0.33f, 0.33f };
        bool transition = false;
        float fb = 0, level = 0;
        Biquad fbHP;

        void prepare (float s, juce::int64 seed)
        {
            sr = s;
            for (auto& l : pt) l.prepare (sr);
            for (auto& l : bb) l.prepare (sr);
            pt[1].rng.setSeed (seed + 7); bb[1].rng.setSeed (seed + 9);
            pt[0].rng.setSeed (seed + 3); bb[0].rng.setSeed (seed + 5);
            tp.prepare (sr, 1.75f, seed);
            ks.prepare (sr);
            fbHP.highpass (320.0f, sr, 0.6f);
            typeStep = 1.0f / (0.012f * sr);
            xfStep = 1.0f / (0.05f * sr);
            reset();
        }
        void reset()
        {
            for (auto& l : pt) l.reset();
            for (auto& l : bb) l.reset();
            tp.reset();
            ks.reset();
            fbHP.reset();
            fb = 0; act = 0; transition = false; xf = 1; waitN = 0;
        }

        // begin on this engine at this time, clean (a new chain: no dip, no fill wait)
        void startAs (int type, float timeSec, bool karplus = false)
        {
            reset();
            activeType = type; typeGain = 1.0f;
            ksMode = karplus;
            if (ksMode) { ks.setPeriod (timeSec, true); lineTime[0] = lineTime[1] = timeSec; return; }
            for (int i = 0; i < 2; ++i) { pt[i].setTime (timeSec, true); bb[i].setTime (timeSec, true); }
            tp.setTime (timeSec, true);
            lineTime[0] = lineTime[1] = timeSec;
        }

        // once per control block
        void block (int type, float tapeAge, float dt)
        {
            tp.age = tapeAge;
            if (ksMode) return;
            if (activeType == tape) tp.wear (dt);
            if (activeType == pt2399 || type == pt2399) { pt[0].updateCoeffs(); pt[1].updateCoeffs(); }
        }

        template <typename Line>
        float runLines (Line (&ln)[2], float in, float timeSec, bool freeze, bool fadeMode)
        {
            if (! fadeMode)
            {
                transition = false;
                ln[act].setTime (timeSec, false);
                lineTime[act] = timeSec;
                return ln[act].process (in, freeze);
            }
            const int other = 1 - act;
            if (! transition && ! freeze && std::abs (timeSec - lineTime[act]) > 0.01f * lineTime[act])
            {
                lineTime[other] = timeSec;
                ln[other].setTime (timeSec, true);
                waitN = (int) (timeSec * sr) + 96;
                transition = true; xf = 0;
            }
            if (transition && freeze && waitN > 0) transition = false;     // don't fade into a half-filled chip

            ln[act].setTime (lineTime[act], false);
            const float ya = ln[act].process (in, freeze);
            if (! transition) return ya;
            const float yb = ln[other].process (in, freeze);
            if (waitN > 0) { --waitN; return ya; }
            xf += xfStep;
            if (xf >= 1.0f) { xf = 1.0f; act = other; transition = false; return yb; }
            return ya * std::cos (xf * pi * 0.5f) + yb * std::sin (xf * pi * 0.5f);
        }

        inline float process (float x, float timeSec, float regen, bool hpf, bool freeze, bool fadeMode, int type)
        {
            if (ksMode)
            {
                // the regen knob is the string's sustain: most of its travel is long rings, the top
                // (past 100%) sings on its own
                ks.setPeriod (timeSec, false);
                const float y = ks.process (x + fb, freeze, fadeMode, type);
                const float f = hpf ? fbHP.process (y) : y;
                fb = freeze ? 0.0f : fastTanh (f) * regen;
                return y;
            }
            if (type != activeType)
            {
                typeGain -= typeStep;
                if (typeGain <= 0.0f)
                {
                    typeGain = 0.0f;
                    activeType = type;
                    // the new engine starts clean, at the current time
                    for (int i = 0; i < 2; ++i) { pt[i].reset(); bb[i].reset(); pt[i].setTime (timeSec, true); bb[i].setTime (timeSec, true); }
                    tp.reset(); tp.setTime (timeSec, true);
                    lineTime[0] = lineTime[1] = timeSec;
                    act = 0; transition = false; fb = 0;
                }
            }
            else typeGain = juce::jmin (1.0f, typeGain + typeStep);

            const float in = x + fb;
            float y;
            if (activeType == tape)
            {
                tp.setTime (timeSec, false);
                y = tp.process (in, freeze, fadeMode);
            }
            else if (activeType == pt2399) y = runLines (pt, in, timeSec, freeze, fadeMode);
            else                            y = runLines (bb, in, timeSec, freeze, fadeMode);

            y *= typeGain;
            const float f = hpf ? fbHP.process (y) : y;
            fb = freeze ? 0.0f : 1.05f * fastTanh (f * regen);
            return y;
        }
    };

    //==========================================================================
    // Lorenz attractor, integrated at control rate. x drives the left cutoff, y the right.
    // speed = how fast it travels, chaos = rho (just above 24.7 it barely holds a lobe,
    // higher gets wilder, with periodic windows up high), shape = beta.
    struct Lorenz
    {
        double x = 0.1, y = 0.0, z = 20.0;
        juce::Random rng { 0x10e7 };

        void step (double dt, double rho, double beta)
        {
            const double sigma = 10.0;
            const int n = juce::jmax (1, (int) std::ceil (dt / 0.004));
            const double h = dt / n;
            for (int i = 0; i < n; ++i)
            {
                auto f = [&] (double a, double b, double c, double& da, double& db, double& dc)
                { da = sigma * (b - a); db = a * (rho - c) - b; dc = a * b - beta * c; };
                double k1x, k1y, k1z, k2x, k2y, k2z, k3x, k3y, k3z, k4x, k4y, k4z;
                f (x, y, z, k1x, k1y, k1z);
                f (x + h * 0.5 * k1x, y + h * 0.5 * k1y, z + h * 0.5 * k1z, k2x, k2y, k2z);
                f (x + h * 0.5 * k2x, y + h * 0.5 * k2y, z + h * 0.5 * k2z, k3x, k3y, k3z);
                f (x + h * k3x, y + h * k3y, z + h * k3z, k4x, k4y, k4z);
                x += h / 6.0 * (k1x + 2 * k2x + 2 * k3x + k4x);
                y += h / 6.0 * (k1y + 2 * k2y + 2 * k3y + k4y);
                z += h / 6.0 * (k1z + 2 * k2z + 2 * k3z + k4z);
            }
            // a whisper of noise so it never settles for good
            x += (rng.nextDouble() - 0.5) * 0.002;
            if (! std::isfinite (x) || ! std::isfinite (y) || ! std::isfinite (z) || std::abs (x) > 1e4) { x = 0.1; y = 0; z = 20; }
        }
        // roughly -1..1
        float outX (double rho, double beta) const { return (float) std::tanh (x / (1.4 * std::sqrt (juce::jmax (1.0, beta * (rho - 1.0))))); }
        float outY (double rho, double beta) const { return (float) std::tanh (y / (1.9 * std::sqrt (juce::jmax (1.0, beta * (rho - 1.0))))); }
        float outZ (double rho) const { return (float) ((z - (rho - 1.0)) / juce::jmax (1.0, rho)); }
    };

    //==========================================================================
    //==========================================================================
    // A small flock (boids): separation, alignment and cohesion in a unit square with soft
    // walls. Each boid steers one poly tap's delay time by its x position, so the taps drift,
    // cluster, scatter and regroup together instead of wobbling independently.
    struct Flock
    {
        static constexpr int n = 14;
        float x[n] {}, y[n] {}, vx[n] {}, vy[n] {};
        float z[n] {}, vz[n] {};   // (in the sphere the flock flies in three dimensions; x still steers the tap times)
        void reset (unsigned seed = 0xb01d5u)
        {
            juce::Random r ((juce::int64) seed);
            for (int i = 0; i < n; ++i)
            {
                x[i] = r.nextFloat() * 2.0f - 1.0f; y[i] = r.nextFloat() * 2.0f - 1.0f; z[i] = r.nextFloat() * 1.2f - 0.6f;
                const float a = r.nextFloat() * twoPi;
                vx[i] = 0.2f * std::cos (a); vy[i] = 0.2f * std::sin (a); vz[i] = 0.08f * (r.nextFloat() * 2.0f - 1.0f);
            }
        }
        void step (float dt)
        {
            constexpr float view = 0.6f, sep = 0.22f, maxV = 0.42f, minV = 0.14f;
            float ax[n], ay[n], az[n];
            for (int i = 0; i < n; ++i)
            {
                float cx = 0, cy = 0, cz = 0, avx = 0, avy = 0, avz = 0, sx = 0, sy = 0, sz = 0; int cnt = 0;
                for (int j = 0; j < n; ++j)
                {
                    if (j == i) continue;
                    const float dx = x[j] - x[i], dy = y[j] - y[i], dz = z[j] - z[i], d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 > view * view) continue;
                    cx += x[j]; cy += y[j]; cz += z[j]; avx += vx[j]; avy += vy[j]; avz += vz[j]; ++cnt;
                    if (d2 < sep * sep) { const float k = 1.0f / juce::jmax (0.0004f, d2); sx -= dx * k; sy -= dy * k; sz -= dz * k; }
                }
                ax[i] = ay[i] = az[i] = 0.0f;
                if (cnt > 0)
                {
                    const float ic = 1.0f / (float) cnt;
                    ax[i] += (cx * ic - x[i]) * 0.5f + (avx * ic - vx[i]) * 0.9f;
                    ay[i] += (cy * ic - y[i]) * 0.5f + (avy * ic - vy[i]) * 0.9f;
                    az[i] += (cz * ic - z[i]) * 0.5f + (avz * ic - vz[i]) * 0.9f;
                }
                ax[i] += sx * 0.012f; ay[i] += sy * 0.012f; az[i] += sz * 0.012f;
                if (z[i] >  0.8f) az[i] -= (z[i] - 0.8f) * 6.0f;
                if (z[i] < -0.6f) az[i] -= (z[i] + 0.6f) * 6.0f;
                // soft walls
                if (x[i] >  0.85f) ax[i] -= (x[i] - 0.85f) * 6.0f;
                if (x[i] < -0.85f) ax[i] -= (x[i] + 0.85f) * 6.0f;
                if (y[i] >  0.85f) ay[i] -= (y[i] - 0.85f) * 6.0f;
                if (y[i] < -0.85f) ay[i] -= (y[i] + 0.85f) * 6.0f;
            }
            for (int i = 0; i < n; ++i)
            {
                vx[i] += ax[i] * dt; vy[i] += ay[i] * dt; vz[i] += az[i] * dt;
                const float sp = std::sqrt (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
                const float lim = juce::jlimit (minV, maxV, sp);
                if (sp > 1.0e-6f) { vx[i] *= lim / sp; vy[i] *= lim / sp; vz[i] *= lim / sp; }
                x[i] = juce::jlimit (-1.0f, 1.0f, x[i] + vx[i] * dt);
                y[i] = juce::jlimit (-1.0f, 1.0f, y[i] + vy[i] * dt);
                z[i] = juce::jlimit (-1.0f, 1.0f, z[i] + vz[i] * dt);
            }
        }
    };

    // One Chloe channel: in att > [filter > tube > delay] or [delay > tube > filter] > out.
    // The delay block holds the main delay and up to seven offset copies (the poly / width taps).
    //
    // Seamless switching: the processing lives in a Chain, and the voice has two. Changing the
    // order, the delay engine or the time mode builds the new setup on the spare chain and
    // crossfades the INPUT into it (60 ms), while the old chain keeps running with its input
    // faded out, so its echoes ring out naturally instead of being cut; it is retired once it
    // has gone quiet (or after 12 s, with a slow fade, e.g. if it was frozen).
    struct ChloeVoice
    {
        struct Flags
        {
            bool hpf = false, freeze = false, tubeOn = true, fade = false;
            // Karplus-Strong time: the delay becomes a string. ksPeriod = the note's period (seconds),
            // ksTap = each poly tap's period
            // as a multiple of the main one (other degrees of the scale: the taps make a chord)
            bool ks = false;
            float ksPeriod = 0.004f;
            float ksTap[7] { 1, 1, 1, 1, 1, 1, 1 };
            int type = DelayCell::pt2399, order = 0, tubeType = 0;
            float width0 = 0, width1 = 0;     // copy level / offset at chunk start / end
            float tapMod[7] { 1, 1, 1, 1, 1, 1, 1 };   // per-tap time multipliers from the flock
        };

        // poly: up to 7 offset copies of the delay (8 taps per side with the main one). They fade
        // in one after another as width goes up; all the way up they smear into a reverb-like
        // cloud. Times are spread irregularly (no two taps line up), and L / R use different sets.
        static constexpr int numCopies = 7;
        static float tapFactor (int k, bool left)
        {
            static const float l[numCopies] = { 1.50f, 0.71f, 1.27f, 0.53f, 1.69f, 0.39f, 1.13f };
            static const float r[numCopies] = { 0.67f, 1.41f, 0.79f, 1.61f, 0.47f, 1.21f, 0.89f };
            return left ? l[k] : r[k];
        }
        // how far tap k is faded in for a given width (tap 0 first, tap 6 last)
        static float tapLevel (int k, float width) { return juce::jlimit (0.0f, 1.0f, width * (float) numCopies - (float) k); }

        struct Chain
        {
            MultiModeFilter filter;
            Tube tube;
            DelayCell delay, copies[numCopies];
            bool copyRunning[numCopies] {};
            int order = 0, type = DelayCell::pt2399;     // the setup this chain was built with
            // the sphere: the dry part and each tap kept apart (tap 0 the main delay, 1..7 the copies),
            // so each can be placed on its own; with the delay first, every tap gets its own filter after it
            float dryPart[64] {}, tapPart[numCopies + 1][64] {};
            MultiModeFilter tapFilter[numCopies + 1];
            bool fade = false, alive = false, ks = false;
            float inGain = 0, outGain = 1, outTarget = 1, env = 0, quiet = 0, age = 0;
            float wetLevel = 0;

            void prepare (float sr, int maxBlock, int seed)
            {
                filter.prepare (sr);
                for (auto& tf : tapFilter) tf.prepare (sr);
                tube.prepare (sr, maxBlock);
                delay.prepare (sr, 1000 + seed * 17);
                for (int k = 0; k < numCopies; ++k) copies[k].prepare (sr, 5000 + seed * 31 + k * 101);
            }
            void reset() { filter.reset(); for (auto& tf : tapFilter) tf.reset(); tube.reset(); delay.reset(); for (auto& c : copies) c.reset(); for (auto& r : copyRunning) r = false; }
            void start (int ord, int ty, bool fd, float timeSec, bool karplus = false)
            {
                reset();
                order = ord; type = ty; fade = fd; ks = karplus;
                delay.startAs (ty, timeSec, ks);
                for (auto& c : copies) c.startAs (ty, timeSec, ks);
                alive = true; inGain = 0; outGain = outTarget = 1; env = quiet = age = 0;
            }
        };

        Chain chains[2];
        int active = 0;
        float sr = 48000.0f, copySign = 1.0f;
        float inLevel = 0, outLevel = 0, wetLevel = 0, meterA = 0.01f;
        float tapeAge = 0;
        static constexpr float tapeLifeSec = 600.0f;   // ~10 minutes from new to ruined
        float buf[64] {}, acc[64] {};
        float dryAcc[64] {}, tapAcc[numCopies + 1][64] {};   // what process() gives the sphere: the dry part and each tap

        void prepare (double s, int maxBlock, int seed = 0)
        {
            sr = (float) s;
            for (int c = 0; c < 2; ++c) chains[c].prepare (sr, maxBlock, seed + c * 7);
            meterA = onePoleCoef (8.0f, sr);
        }
        void reset()
        {
            for (auto& c : chains) { c.reset(); c.alive = false; }
            chains[active].alive = true; chains[active].inGain = 1.0f;
        }
        void refreshTape() { tapeAge = 0; for (auto& ch : chains) { ch.delay.tp.refresh(); for (auto& c : ch.copies) c.tp.refresh(); } }
        void setTapeAge (float a)
        {
            tapeAge = juce::jlimit (0.0f, 1.0f, a);
            for (auto& ch : chains)
            {
                ch.delay.tp.age = tapeAge; ch.delay.tp.seedDamage();
                for (auto& c : ch.copies) { c.tp.age = tapeAge; c.tp.seedDamage(); }
            }
        }
        float glow() const { return chains[active].tube.glow; }
        float latency() const { return chains[0].tube.latency(); }

        // p0/p1: parameter values (real units, see chl::ModParam) at chunk start/end; n <= 64
        void process (float* io, int n, const float* p0, const float* p1, const Flags& f)
        {
            const float inv = 1.0f / (float) n;
            auto lerp = [&] (int idx, int i) { return p0[idx] + (p1[idx] - p0[idx]) * ((float) (i + 1) * inv); };
            auto dbToGain = [] (float db) { return db <= -47.9f ? 0.0f : std::exp2 (db * 0.16609640474f); };
            const float dt = (float) n / sr;

            // a new setup asked for: build it on the spare chain (once that one is free)
            {
                auto& a = chains[active];
                const float t0 = f.ks ? f.ksPeriod : p0[vTime];
                if (! chains[active].alive) { a.start (f.order, f.type, f.fade, t0, f.ks); a.inGain = 1.0f; }
                else if (f.order != a.order || f.type != a.type || f.fade != a.fade || f.ks != a.ks)
                {
                    auto& spare = chains[1 - active];
                    if (! spare.alive)
                    {
                        spare.start (f.order, f.type, f.fade, t0, f.ks);
                        active = 1 - active;
                    }
                    else spare.outTarget = 0.0f;      // still ringing from an earlier switch: hurry it out
                }
            }

            if (f.type == DelayCell::tape && ! f.ks) tapeAge = juce::jmin (1.0f, tapeAge + dt / tapeLifeSec);

            // input level
            for (int i = 0; i < n; ++i)
            {
                io[i] *= dbToGain (lerp (vIn, i));
                inLevel += meterA * (std::abs (io[i]) - inLevel);
                acc[i] = 0.0f;
                dryAcc[i] = 0.0f;
                for (auto& t : tapAcc) t[i] = 0.0f;
            }

            const float inStep = dt / 0.06f, outFast = dt / 0.04f, outSlow = dt / 1.0f;
            for (int c = 0; c < 2; ++c)
            {
                auto& ch = chains[c];
                if (! ch.alive) continue;
                const bool isActive = c == active;
                // tape hiss: up while this is the running chain, down once it has been switched away from
                ch.delay.tp.hissTarget = isActive ? 1.0f : 0.0f;
                for (auto& cp : ch.copies) cp.tp.hissTarget = ch.delay.tp.hissTarget;
                const float in0 = ch.inGain;
                ch.inGain = juce::jlimit (0.0f, 1.0f, ch.inGain + (isActive ? inStep : -inStep));
                // ringing out: retire once quiet for 0.3 s; after 12 s (e.g. frozen) fade it away
                if (! isActive)
                {
                    ch.age += dt;
                    if (ch.age > 12.0f) ch.outTarget = juce::jmin (ch.outTarget, 0.0f);
                }
                const float out0 = ch.outGain;
                const float ostep = (ch.outTarget < ch.outGain && ch.age > 12.0f) ? outSlow : outFast;
                ch.outGain += juce::jlimit (-ostep, ostep, ch.outTarget - ch.outGain);
                for (int i = 0; i < n; ++i)
                {
                    const float k = (float) (i + 1) * inv;
                    const float g = in0 + (ch.inGain - in0) * k;
                    buf[i] = io[i] * (g * g * (3.0f - 2.0f * g));         // s-curve crossfade
                }
                runChain (ch, buf, n, p0, p1, f, lerp, inv, dt);
                for (int i = 0; i < n; ++i)
                {
                    const float og = out0 + (ch.outGain - out0) * ((float) (i + 1) * inv);
                    acc[i] += buf[i] * og;
                    dryAcc[i] += ch.dryPart[i] * og;
                    for (int k = 0; k <= numCopies; ++k) tapAcc[k][i] += ch.tapPart[k][i] * og;
                    if (! isActive) ch.env += 0.002f * (std::abs (buf[i] * og) - ch.env);
                }
                if (! isActive)
                {
                    ch.quiet = (ch.inGain <= 0.0f && ch.env < 1.0e-4f) ? ch.quiet + dt : 0.0f;
                    if (ch.quiet > 0.3f || (ch.outGain <= 0.0f && ch.outTarget <= 0.0f)) ch.alive = false;
                }
            }
            wetLevel = chains[active].wetLevel;

            for (int i = 0; i < n; ++i)
            {
                const float og = dbToGain (lerp (vOut, i));
                const float y = acc[i] * og;
                io[i] = y;
                dryAcc[i] *= og;
                for (auto& t : tapAcc) t[i] *= og;
                outLevel += meterA * (std::abs (y) - outLevel);
            }
        }

        template <typename Lerp>
        void runChain (Chain& ch, float* x, int n, const float* p0, const float* p1, const Flags& f, Lerp& lerp, float inv, float dt)
        {
            ch.delay.block (ch.type, tapeAge, dt);
            bool tapOn[numCopies];
            bool copyOn = false;
            for (int k = 0; k < numCopies; ++k)
            {
                tapOn[k] = tapLevel (k, f.width0) > 0.0005f || tapLevel (k, f.width1) > 0.0005f;
                if (tapOn[k] && ! ch.copyRunning[k]) ch.copies[k].startAs (ch.type, ch.ks ? f.ksPeriod * f.ksTap[k] : lerp (vTime, 0), ch.ks);
                ch.copyRunning[k] = tapOn[k];
                if (tapOn[k]) { ch.copies[k].block (ch.type, tapeAge, dt); copyOn = true; }
            }
            auto runFilter = [&]
            {
                for (int i = 0; i < n; ++i)
                    x[i] = ch.filter.process (x[i], lerp (vCut, i), lerp (vRes, i), lerp (vMorph, i));
            };
            // the string's sustain: most of the regen knob's travel is long rings; past 100% it sings on its own
            auto ksRegen = [] (float r) { return r < 1.0f ? std::pow (juce::jmax (0.0f, r), 0.08f) : r; };
            const float ksR0 = ksRegen (p0[vRegen]), ksR1 = ksRegen (p1[vRegen]);
            auto runDelay = [&]
            {
                for (int i = 0; i < n; ++i)
                {
                    const float dry = x[i];
                    // (a chain built as a string keeps the last note if the switch has since gone off)
                    const float t = ch.ks ? f.ksPeriod : lerp (vTime, i);
                    const float rg = ch.ks ? ksR0 + (ksR1 - ksR0) * ((float) (i + 1) * inv) : lerp (vRegen, i);
                    float wetv = ch.delay.process (dry, t, rg, f.hpf, f.freeze, ch.fade, ch.type);
                    const float mainWet = wetv;
                    float cwk[numCopies] {};
                    float norm = 1.0f;
                    if (copyOn)
                    {
                        // the taps: same engine, each at its own offset time (growing as it fades
                        // in) and a little less regen, so the cloud decays rather than builds
                        const float wd = f.width0 + (f.width1 - f.width0) * ((float) (i + 1) * inv);
                        float sum = wetv, lv = 0.0f;
                        for (int k = 0; k < numCopies; ++k)
                        {
                            if (! tapOn[k]) continue;
                            const float l = tapLevel (k, wd);
                            const float tk = ch.ks ? f.ksTap[k]
                                                   : (1.0f + (tapFactor (k, copySign > 0) - 1.0f) * juce::jmax (l, 0.001f)) * f.tapMod[k];
                            const float cw = ch.copies[k].process (dry, t * tk, rg * (ch.ks ? 1.0f - 0.0015f * (float) (k + 1) : 0.84f - 0.03f * (float) k), f.hpf, f.freeze, ch.fade, ch.type);
                            sum += cw * l;
                            cwk[k] = cw * l;
                            lv += l;
                        }
                        norm = 1.0f / (1.0f + 0.3f * lv);
                        wetv = sum * norm;
                    }
                    ch.wetLevel += meterA * (std::abs (wetv) - ch.wetLevel);
                    const float m = lerp (vWet, i);
                    const float cs = std::cos (m * pi * 0.5f), sn = std::sin (m * pi * 0.5f);
                    x[i] = dry * cs + wetv * sn;
                    ch.dryPart[i] = dry * cs;
                    ch.tapPart[0][i] = mainWet * norm * sn;
                    for (int k = 0; k < numCopies; ++k) ch.tapPart[k + 1][i] = cwk[k] * norm * sn;
                }
            };
            if (ch.order == 0) { runFilter(); ch.tube.process (x, n, p0[vDrive], p1[vDrive], f.tubeOn, f.tubeType); runDelay(); }
            else
            {
                // delay first: the dry part goes on through the tube and the filter; each tap through its
                // own filter (the same settings), so the taps stay apart for the sphere
                runDelay();
                for (int i = 0; i < n; ++i) x[i] = ch.dryPart[i];
                ch.tube.process (x, n, p0[vDrive], p1[vDrive], f.tubeOn, f.tubeType);
                runFilter();
                for (int i = 0; i < n; ++i) ch.dryPart[i] = x[i];
                for (int k = 0; k <= numCopies; ++k)
                    for (int i = 0; i < n; ++i)
                    {
                        ch.tapPart[k][i] = ch.tapFilter[k].process (ch.tapPart[k][i], lerp (vCut, i), lerp (vRes, i), lerp (vMorph, i));
                        x[i] += ch.tapPart[k][i];
                    }
            }
        }
    };
}
