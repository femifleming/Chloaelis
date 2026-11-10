#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ChlAudio.h"

using namespace juce;

static_assert ((int) chl::inAtt == (int) chl::dsp::vIn && (int) chl::cutoff == (int) chl::dsp::vCut && (int) chl::res == (int) chl::dsp::vRes && (int) chl::morph == (int) chl::dsp::vMorph
               && (int) chl::drive == (int) chl::dsp::vDrive && (int) chl::time == (int) chl::dsp::vTime && (int) chl::regen == (int) chl::dsp::vRegen
               && (int) chl::wet == (int) chl::dsp::vWet && (int) chl::out == (int) chl::dsp::vOut, "voice parameter slots must match the parameter enum");

ChloaelisProcessor::ChloaelisProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", AudioChannelSet::ambisonic (3), true)),
      apvts (*this, nullptr, "CHLOAELIS", chl::createLayout())
{
    for (int s = 0; s < chl::numSides; ++s)
    {
        for (int p = 0; p < chl::numMod; ++p)
            pRaw[s][p] = apvts.getRawParameterValue (chl::pid (s, p));
        pHpf[s]     = apvts.getRawParameterValue (chl::hpfId (s));
        pFreeze[s]  = apvts.getRawParameterValue (chl::freezeId (s));
        pTubeOn[s]  = apvts.getRawParameterValue (chl::tubeOnId (s));
        pType[s]    = apvts.getRawParameterValue (chl::delayTypeId (s));
        pTMode[s]   = apvts.getRawParameterValue (chl::timeModeId (s));
        pTubeType[s] = apvts.getRawParameterValue (chl::tubeTypeId (s));
        voices[s].copySign = s == 0 ? 1.0f : -1.0f;
        lfoPhase[s][0] = 0.0f; lfoPhase[s][1] = 0.33f; lfoPhase[s][2] = 0.66f;
    }
    pLzSpeed = apvts.getRawParameterValue (chl::lzSpeedId);
    pLzChaos = apvts.getRawParameterValue (chl::lzChaosId);
    pLzShape = apvts.getRawParameterValue (chl::lzShapeId);
    pLzDepth = apvts.getRawParameterValue (chl::lzDepthId);
    pWidth   = apvts.getRawParameterValue (chl::widthId);
    pOrder   = apvts.getRawParameterValue (chl::orderId);
    pSrcSel   = apvts.getRawParameterValue (chl::srcSelId);
    pSrcLevel = apvts.getRawParameterValue (chl::srcLevelId);
    pMasterFb = apvts.getRawParameterValue (chl::masterFbId);
    pFlock    = apvts.getRawParameterValue (chl::flockId);
    pSpace    = apvts.getRawParameterValue (chl::spaceId);
    flock.reset();

    // the recordings are compiled into the plugin (Ogg Vorbis: decodes the same on every platform)
    auto makeReader = [] (const char* data, int size)
    {
        OggVorbisAudioFormat fmt;
        return std::unique_ptr<AudioFormatReader> (fmt.createReaderFor (new MemoryInputStream (data, (size_t) size, false), true));
    };
    // [0] baserecording1, its long middle loop: the file holds 3:30-7:26 of the original, and
    //     the loop runs 3:36-7:25.5 (seam chosen by spectral matching), 4 s crossfade
    streams[0].setup (makeReader (ChlAudio::baseloop_ogg, ChlAudio::baseloop_oggSize), 6.0, 235.5, 4.0, 6.0);
    // [1] hydrophoneSTEREOjune2026_3, the whole thing looping; ~3.5 dB quieter, so trimmed up
    streams[1].setup (makeReader (ChlAudio::hydrophone3_ogg, ChlAudio::hydrophone3_oggSize), 4.0, -1.0, 4.0, 0.0, 3.5f);
    srcThread.startThread (Thread::Priority::low);

    loadLastState();
    for (int p = 0; p < chl::numMod; ++p)
        ranges[p] = chl::rangeFor (p);

    for (auto& side : paramNorm) for (auto& a : side) a = 0.0f;
    for (auto* arr : { &tubeGlow, &inLevel, &outLevel, &wetLevel, &frozen, &randHeld, &tapeAge })
        for (auto& a : *arr) a = 0.0f;
    for (auto& d : lfoValue) for (auto& a : d) a = 0.0f;
    for (auto& b : refreshTape) b = false;
}

bool ChloaelisProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // 3rd-order Ambisonics out (16 plain channels too, except as an Audio Unit, which publishes only the ambisonic layout)
    const auto out = layouts.getMainOutputChannelSet();
    if (out != AudioChannelSet::ambisonic (3) && ! (out == AudioChannelSet::discreteChannels (16) && wrapperType != wrapperType_AudioUnit))
        return false;
    const auto in = layouts.getMainInputChannelSet();
    return in == AudioChannelSet::mono() || in == AudioChannelSet::stereo();
}

void ChloaelisProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    const int nb = jmax (1, samplesPerBlock);
    for (auto& side : dist) for (auto& d : side) d.prepare (sampleRate, nb);
    parts.setSize (chl::numSides * kPoints, nb, false, true, true);
    bus.setSize (fp::ambi::kCh, nb, false, true, true);
    energy.prepare (sampleRate);
    leveler.prepare (sampleRate);
    limiter.prepare (sampleRate);
    for (int s = 0; s < chl::numSides; ++s)
    {
        const float age = voices[s].tapeAge;
        voices[s].prepare (sampleRate, chunk, s);
        voices[s].reset();
        voices[s].setTapeAge (age);
    }
    widthCur = pWidth->load();
    readTargets (cur);
    modulate (cur, 0);
    setLatencySamples ((int) std::round (voices[0].latency()) + limiter.latency());

    for (auto& st : streams)
    {
        srcThread.removeTimeSliceClient (&st);
        st.prepare (sampleRate);
        srcThread.addTimeSliceClient (&st);
    }
    srcSelGain[0] = pSrcSel->load() > 0.5f ? 0.0f : 1.0f;
    srcSelGain[1] = 1.0f - srcSelGain[0];
    srcLevelCur = pSrcLevel->load() * pSrcLevel->load();
    fbCur = 0.0f;
    for (auto& r : fbRing) std::fill (std::begin (r), std::end (r), 0.0f);
    for (int s = 0; s < chl::numSides; ++s) fbDcX[s] = fbDcY[s] = 0.0f;
}

void ChloaelisProcessor::readTargets (float (&dst)[chl::numSides][chl::numMod])
{
    for (int s = 0; s < chl::numSides; ++s)
        for (int p = 0; p < chl::numMod; ++p)
            dst[s][p] = pRaw[s][p]->load();
}

// Lorenz -> cutoffs, LFOs -> mode / time / drive (unipolar, upwards), random freeze clocks
void ChloaelisProcessor::modulate (float (&dst)[chl::numSides][chl::numMod], int len)
{
    const float dt = (float) len / (float) sr;

    const double chaos = pLzChaos->load(), rho = 24.5 + chaos * chaos * 155.5, beta = pLzShape->load();
    const double speed = 0.03 * std::exp2 (pLzSpeed->load() * 9.0);     // attractor time units per second
    if (len > 0) lorenz.step (speed * dt, rho, beta);
    const float lo[2] = { lorenz.outX (rho, beta), lorenz.outY (rho, beta) };
    lzN[0] = lo[0]; lzN[1] = lo[1]; lzN[2] = jlimit (-1.0f, 1.0f, lorenz.outZ (rho));
    lzX.store ((float) lorenz.x, std::memory_order_relaxed); lzY.store ((float) lorenz.y, std::memory_order_relaxed); lzZ.store ((float) lorenz.z, std::memory_order_relaxed);
    lzOut[0].store (lo[0], std::memory_order_relaxed); lzOut[1].store (lo[1], std::memory_order_relaxed);
    const float depth = pLzDepth->load();

    static constexpr int dest[3] = { chl::morph, chl::time, chl::drive };
    static constexpr int rate[3] = { chl::lmRate, chl::ltRate, chl::ldRate };
    static constexpr int amt[3]  = { chl::lmDepth, chl::ltDepth, chl::ldDepth };

    for (int s = 0; s < chl::numSides; ++s)
    {
        auto* d = dst[s];
        if (depth > 0.0f)
            d[chl::cutoff] = jlimit (20.0f, 20000.0f, d[chl::cutoff] * std::exp2 (depth * 3.5f * lo[s]));

        for (int k = 0; k < 3; ++k)
        {
            float& ph = lfoPhase[s][k];
            ph += d[rate[k]] * dt;
            ph -= std::floor (ph);
            const float tri = 1.0f - std::abs (2.0f * ph - 1.0f);
            lfoValue[k][(size_t) s].store (tri, std::memory_order_relaxed);
            if (d[amt[k]] > 0.0f)
            {
                const int p = dest[k];
                const float nrm = jlimit (0.0f, 1.0f, ranges[p].convertTo0to1 (d[p]) + d[amt[k]] * tri);
                d[p] = ranges[p].convertFrom0to1 (nrm);
            }
        }

        // random freeze: a slow random binary oscillator; the knob is the chance of holding
        const float r = d[chl::rnd];
        rfTimer[s] -= dt;
        if (r < 0.001f) { rfOn[s] = false; rfTimer[s] = 0.0f; }
        else if (rfTimer[s] <= 0.0f)
        {
            if (rfOn[s])                       { rfOn[s] = false; rfTimer[s] = 0.3f + rng.nextFloat() * 2.5f * (1.1f - r); }
            else if (rng.nextFloat() < r)      { rfOn[s] = true;  rfTimer[s] = 0.4f + rng.nextFloat() * 3.5f * (0.3f + r); }
            else                                 rfTimer[s] = 0.25f + rng.nextFloat() * 1.2f;
        }

        for (int p = 0; p < chl::numMod; ++p)
            paramNorm[(size_t) s][(size_t) p].store (ranges[p].convertTo0to1 (d[p]), std::memory_order_relaxed);
    }
}

ChloaelisProcessor::~ChloaelisProcessor()
{
    saveLastState();
    srcThread.removeAllClients();
    srcThread.stopThread (2000);
}

void ChloaelisProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numIn = jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    const int numOut = buffer.getNumChannels();
    if (n == 0 || numOut == 0) return;

    const float* inL = numIn > 0 ? buffer.getReadPointer (0) : nullptr;
    const float* inR = numIn > 1 ? buffer.getReadPointer (1) : inL;
    if (parts.getNumSamples() < n) { parts.setSize (chl::numSides * kPoints, n, false, true, true); bus.setSize (fp::ambi::kCh, n, false, true, true); }

    chl::dsp::ChloeVoice::Flags fl[2];
    bool manualFreeze[2];
    for (int s = 0; s < chl::numSides; ++s)
    {
        fl[s].hpf    = pHpf[s]->load() > 0.5f;
        manualFreeze[s] = pFreeze[s]->load() > 0.5f;
        fl[s].tubeOn = pTubeOn[s]->load() > 0.5f;
        fl[s].type   = jlimit (0, 2, (int) std::lround (pType[s]->load()));
        fl[s].fade   = pTMode[s]->load() > 0.5f;
        fl[s].order  = pOrder->load() > 0.5f ? 1 : 0;
        fl[s].tubeType = jlimit (0, 3, (int) std::lround (pTubeType[s]->load()));

        if (refreshTape[(size_t) s].exchange (false)) voices[s].refreshTape();
        const float pa = pendingAge[s].exchange (-1.0f);
        if (pa >= 0.0f) voices[s].setTapeAge (pa);
    }

    float tmp[2][chunk];
    float target[chl::numSides][chl::numMod];
    const float widthTarget = pWidth->load();
    const int srcSel = pSrcSel->load() > 0.5f ? 1 : 0;
    const float srcLevelTarget = pSrcLevel->load() * pSrcLevel->load();      // an audio taper for the fade
    const float fbTarget = pMasterFb->load() * 1.1f;                          // a little past 1: it can sing
    const float selStep = (float) chunk / (0.06f * (float) sr), lvlCoef = 1.0f - std::exp (-(float) chunk / (0.02f * (float) sr));

    for (int start = 0; start < n; start += chunk)
    {
        const int len = jmin (chunk, n - start);
        readTargets (target);     // parameter changes ramp over one 16-sample chunk
        modulate (target, len);
        const float w0 = widthCur;
        widthCur += jlimit (-0.004f, 0.004f, widthTarget - widthCur);

        for (int i = 0; i < len; ++i)
        {
            tmp[0][i] = inL ? inL[start + i] : 0.0f;
            tmp[1][i] = inR ? inR[start + i] : 0.0f;
        }

        // the built-in recording, its left into the left strip and its right into the right
        {
            const float l0 = srcLevelCur;
            srcLevelCur += (srcLevelTarget - srcLevelCur) * lvlCoef;
            for (int k = 0; k < 2; ++k)
            {
                const float g0 = srcSelGain[k];
                srcSelGain[k] += jlimit (-selStep, selStep, (k == srcSel ? 1.0f : 0.0f) - srcSelGain[k]);
                // the chosen one always runs (the recording keeps moving while faded out); the
                // other only until its crossfade has finished
                if (k == srcSel || g0 > 0.0f)
                    streams[k].pullAdd (tmp[0], tmp[1], len, g0 * l0, srcSelGain[k] * srcLevelCur);
            }
        }

        // the vessel: the dropped file, looped between its rings (equal-power seam)
        if (auto* p = vesselPending.exchange (nullptr)) { vesselCur = p; vesselPhase = -1.0; }
        if (vesselCur != nullptr && vesselCur->audio.getNumSamples() > 64)
        {
            const auto& au = vesselCur->audio;
            const int N = au.getNumSamples();
            const double step = vesselCur->rate / sr;
            double a = jlimit (0.0, 1.0, (double) vesselStart.load()) * (N - 1), b = jlimit (0.0, 1.0, (double) vesselEnd.load()) * (N - 1);
            if (b < a) std::swap (a, b);
            b = jmax (b, jmin ((double) (N - 2), a + 0.05 * vesselCur->rate));
            const double xf = jmax (1.0, jmin (0.02 * vesselCur->rate, (b - a) * 0.25));
            if (vesselPhase < a || vesselPhase >= b) vesselPhase = a;
            const float vesselTarget = vesselLevel.load() * vesselLevel.load();
            const float g0 = vesselGain;
            vesselGain += (vesselTarget - vesselGain) * lvlCoef;
            auto at = [&] (int ch, double x)
            {
                const int i = jlimit (0, N - 2, (int) x);
                const float f = (float) (x - i);
                const float* d = au.getReadPointer (ch);
                return d[i] + (d[i + 1] - d[i]) * f;
            };
            if (g0 > 0.00001f || vesselGain > 0.00001f)
                for (int i = 0; i < len; ++i)
                {
                    float l = at (0, vesselPhase), r = at (1, vesselPhase);
                    if (vesselPhase > b - xf)
                    {
                        const double into = vesselPhase - (b - xf);
                        const float t = (float) (into / xf);
                        const float co = std::cos (t * MathConstants<float>::halfPi), si = std::sin (t * MathConstants<float>::halfPi);
                        l = l * co + at (0, a + into) * si;
                        r = r * co + at (1, a + into) * si;
                    }
                    const float g = g0 + (vesselGain - g0) * (float) (i + 1) / (float) len;
                    tmp[0][i] += l * g; tmp[1][i] += r * g;
                    vesselPhase += step;
                    if (vesselPhase >= b) vesselPhase = a + xf;
                }
            else
            {
                vesselPhase += step * len;
                if (vesselPhase >= b) vesselPhase = a + std::fmod (vesselPhase - a, jmax (1.0, b - a));
            }
            vesselPos.store ((float) (vesselPhase / (N - 1)), std::memory_order_relaxed);
        }

        // master feedback, crossed: what the RIGHT side put out 16 samples ago goes into the
        // left input and the left output into the right, DC-blocked and soft-limited
        {
            const float f0 = fbCur;
            fbCur += jlimit (-0.002f, 0.002f, fbTarget - fbCur);
            if (f0 > 0.0001f || fbCur > 0.0001f)
                for (int s = 0; s < chl::numSides; ++s)
                    for (int i = 0; i < len; ++i)
                    {
                        const float y = fbRing[1 - s][(fbWrite - chunk + i) & 63];
                        fbDcY[s] = y - fbDcX[s] + 0.9995f * fbDcY[s];
                        fbDcX[s] = y;
                        const float g = f0 + (fbCur - f0) * (float) (i + 1) / (float) len;
                        tmp[s][i] += std::tanh (fbDcY[s] * g);
                    }
        }

        // the flock moves the poly taps' times: up to ~0.9 octave either way at full depth
        flock.step ((float) len / (float) sr);
        flockCur += (pFlock->load() - flockCur) * 0.01f;
        for (int s = 0; s < chl::numSides; ++s)
            for (int k = 0; k < 7; ++k)
                fl[s].tapMod[k] = flockCur > 0.0005f ? std::exp2 (flockCur * 0.9f * flock.x[s * 7 + k]) : 1.0f;

        for (int s = 0; s < chl::numSides; ++s)
        {
            fl[s].freeze = manualFreeze[s] || rfOn[s];
            fl[s].width0 = w0; fl[s].width1 = widthCur;
            voices[s].process (tmp[s], len, cur[s], target[s], fl[s]);
            std::copy (target[s], target[s] + chl::numMod, cur[s]);
        }

        for (int s = 0; s < chl::numSides; ++s)
            for (int i = 0; i < len; ++i)
                fbRing[s][(fbWrite + i) & 63] = tmp[s][i];
        fbWrite = (fbWrite + len) & 63;

        // into the sphere: each side's dry sound and each of its taps, kept apart
        for (int s = 0; s < chl::numSides; ++s)
        {
            std::copy (voices[s].dryAcc, voices[s].dryAcc + len, parts.getWritePointer (s * kPoints) + start);
            for (int k = 0; k <= chl::dsp::ChloeVoice::numCopies; ++k)
                std::copy (voices[s].tapAcc[k], voices[s].tapAcc[k] + len, parts.getWritePointer (s * kPoints + 1 + k) + start);
        }
    }

    // where everything is: the dry sounds front left and front right, the main echoes on the attractor
    // (the right one mirrored through you), the poly taps on their birds; all scaled by distance
    {
        // positions are fractions of the bubble; the bubble's size (the Distance knob) is in metres
        const float space = 1.0f;
        const float bubble = bubbleNow.load() + (bubbleMetres (pSpace->load()) - bubbleNow.load()) * jmin (1.0f, (float) n / (float) (0.15 * sr));
        bubbleNow.store (bubble);
        for (int s = 0; s < chl::numSides; ++s)
        {
            const float side = s == 0 ? 1.0f : -1.0f;
            where[(size_t) (s * kPoints)] = fp::ambi::fromAzEl (side * MathConstants<float>::pi / 3.0f, 0.0f) * (0.45f * space);
            where[(size_t) (s * kPoints + 1)] = lorenzPoint (lzN[0], lzN[1], lzN[2], s) * space;
            // (the flock bunches up as flocks do: it's kept centred on you, so it gathers and scatters round you
            // rather than off to one side, and opened out to fill the sphere)
            fp::ambi::Vec3 centre;
            for (int b = 0; b < chl::dsp::Flock::n; ++b) centre += fp::ambi::Vec3 { flock.x[b], flock.y[b], flock.z[b] };
            centre *= 1.0f / (float) chl::dsp::Flock::n;
            for (int k = 0; k < chl::dsp::ChloeVoice::numCopies; ++k)
            {
                const int b = s * 7 + k;
                fp::ambi::Vec3 p = (fp::ambi::Vec3 { flock.x[b], flock.y[b], flock.z[b] } - centre * 0.85f) * 1.6f;
                const float l = p.length();
                if (l > 1.0f) p = p * (1.0f / l);
                where[(size_t) (s * kPoints + 2 + k)] = p * space;
            }
        }
        bus.clear (0, n);
        float* const* bw = bus.getArrayOfWritePointers();
        // both sides meet in the same channels: each at -3 dB, so the two together come out about as loud as one did
        parts.applyGain (0, n, 0.70710678f);
        for (int s = 0; s < chl::numSides; ++s)
            for (int j = 0; j < kPoints; ++j)
            {
                const float* x = parts.getReadPointer (s * kPoints + j);
                const auto p = where[(size_t) (s * kPoints + j)];
                dist[s][j].process (x, n, p, bubble, bw, nullptr, 0.0f);
                auto& pv = points[(size_t) s][(size_t) j];
                pv.x.store (p.x, std::memory_order_relaxed); pv.y.store (p.y, std::memory_order_relaxed); pv.z.store (p.z, std::memory_order_relaxed);
                const float lv = parts.getRMSLevel (s * kPoints + j, 0, n);
                pv.level.store (pv.level.load (std::memory_order_relaxed) * 0.7f + 0.3f * lv, std::memory_order_relaxed);
            }
        // (never let a NaN through)
        for (int c = 0; c < fp::ambi::kCh; ++c)
            for (int i = 0; i < n; ++i) if (! std::isfinite (bw[c][i])) bw[c][i] = 0.0f;
        // the 16 channels, linked: never clips (a small bubble brings everything close and loud)
        leveler.process (bw, fp::ambi::kCh, n);
        limiter.process (bw, fp::ambi::kCh, n);
        energy.process (bus.getArrayOfReadPointers(), n);
        outChannels.store (numOut, std::memory_order_relaxed);
        for (int c = 0; c < numOut; ++c)
            if (c < fp::ambi::kCh) buffer.copyFrom (c, 0, bus.getReadPointer (c), n);
            else buffer.clear (c, 0, n);
    }

    for (int i = 0; i < chl::dsp::Flock::n; ++i)
    {
        flockX[(size_t) i].store (flock.x[i], std::memory_order_relaxed);
        flockY[(size_t) i].store (flock.y[i], std::memory_order_relaxed);
        flockZ[(size_t) i].store (flock.z[i], std::memory_order_relaxed);
    }
    for (int s = 0; s < chl::numSides; ++s)
    {
        tubeGlow[(size_t) s].store (voices[s].glow(), std::memory_order_relaxed);
        inLevel[(size_t) s].store (voices[s].inLevel, std::memory_order_relaxed);
        outLevel[(size_t) s].store (voices[s].outLevel, std::memory_order_relaxed);
        wetLevel[(size_t) s].store (voices[s].wetLevel, std::memory_order_relaxed);
        frozen[(size_t) s].store (fl[s].freeze ? 1.0f : 0.0f, std::memory_order_relaxed);
        randHeld[(size_t) s].store (rfOn[s] ? 1.0f : 0.0f, std::memory_order_relaxed);
        tapeAge[(size_t) s].store (voices[s].tapeAge, std::memory_order_relaxed);
    }
}

ValueTree ChloaelisProcessor::stateWithExtras()
{
    auto state = apvts.copyState();
    // the tape's wear is part of the instrument's state, not a parameter
    state.setProperty ("tapeAgeL", tapeAge[0].load(), nullptr);
    state.setProperty ("tapeAgeR", tapeAge[1].load(), nullptr);
    if (vesselFile.existsAsFile())
    {
        state.setProperty ("vesselPath", vesselFile.getFullPathName(), nullptr);
        state.setProperty ("vesselStart", vesselStart.load(), nullptr);
        state.setProperty ("vesselEnd", vesselEnd.load(), nullptr);
    }
    state.setProperty ("vesselLevel", vesselLevel.load(), nullptr);
    return state;
}

void ChloaelisProcessor::applyState (const ValueTree& tree)
{
    if (! tree.hasType (apvts.state.getType())) return;
    for (int s = 0; s < chl::numSides; ++s)
    {
        const float a = (float) tree.getProperty (s == 0 ? "tapeAgeL" : "tapeAgeR", 0.0f);
        pendingAge[s].store (jlimit (0.0f, 1.0f, a));
        tapeAge[(size_t) s].store (a);
    }
    vesselLevel.store ((float) tree.getProperty ("vesselLevel", 0.0f));
    const String vp = tree.getProperty ("vesselPath", String());
    if (isStandalone() && vp.isNotEmpty() && File (vp).existsAsFile())
    {
        vesselStart.store ((float) tree.getProperty ("vesselStart", 0.0f));
        vesselEnd.store ((float) tree.getProperty ("vesselEnd", 1.0f));
        if (File (vp) != vesselFile) loadVesselFile (File (vp), true);
    }
    apvts.replaceState (tree);
}

bool ChloaelisProcessor::loadVesselFile (const File& f, bool keepLoop)
{
    AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples < 256) return false;
    auto clip = std::make_shared<VesselClip>();
    clip->rate = r->sampleRate;
    clip->name = f.getFileNameWithoutExtension();
    const int n = (int) std::min<int64> (r->lengthInSamples, (int64) (r->sampleRate * 600.0));   // up to 10 minutes
    clip->audio.setSize (2, n);
    r->read (&clip->audio, 0, n, 0, true, true);
    if (r->numChannels == 1) clip->audio.copyFrom (1, 0, clip->audio, 0, 0, n);
    constexpr int slices = 400;
    clip->peaks.assign (slices, 0.0f);
    for (int k = 0; k < slices; ++k)
    {
        const int s0 = (int) ((int64) k * n / slices), s1 = (int) ((int64) (k + 1) * n / slices);
        float m = 0.0f;
        for (int c = 0; c < 2; ++c) m = jmax (m, clip->audio.getMagnitude (c, s0, jmax (1, s1 - s0)));
        clip->peaks[(size_t) k] = m;
    }
    if (! keepLoop) { vesselStart.store (0.0f); vesselEnd.store (1.0f); }
    vesselFile = f;
    vesselKeep.push_back (clip);
    vesselUi = clip;
    vesselPending.store (clip.get());
    // the audio thread only ever holds the newest clip or the one before: keep the last three
    while (vesselKeep.size() > 3) vesselKeep.erase (vesselKeep.begin());
    return true;
}

void ChloaelisProcessor::getStateInformation (MemoryBlock& destData)
{
    if (auto xml = stateWithExtras().createXml())
        copyXmlToBinary (*xml, destData);
}

void ChloaelisProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        applyState (ValueTree::fromXml (*xml));
}

File ChloaelisProcessor::lastStateFile()
{
    // macOS: ~/Library/Application Support/Chloaelis, Windows: %APPDATA%\Chloaelis,
    // Linux: ~/.config/Chloaelis
    auto base = File::getSpecialLocation (File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Chloaeilius").getChildFile ("LastState.xml");   // (its own, apart from Chloaelis's)
}

void ChloaelisProcessor::saveLastState()
{
    if (std::getenv ("CHL_NO_LASTSTATE") != nullptr) return;
    if (auto xml = stateWithExtras().createXml())
    {
        auto f = lastStateFile();
        f.getParentDirectory().createDirectory();
        xml->writeTo (f);
    }
}

void ChloaelisProcessor::loadLastState()
{
    if (std::getenv ("CHL_NO_LASTSTATE") != nullptr) return;
    if (auto xml = XmlDocument::parse (lastStateFile()))
        applyState (ValueTree::fromXml (*xml));
}

AudioProcessorEditor* ChloaelisProcessor::createEditor() { return new ChloaelisEditor (*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ChloaelisProcessor(); }

// the attractor's butterfly, fitted into the sphere; the right side's echo is the left's mirrored through you
fp::ambi::Vec3 ChloaelisProcessor::lorenzPoint (float x, float y, float z, int side) noexcept
{
    fp::ambi::Vec3 p { 0.7f * y, 0.7f * x, 0.5f * z };   // (kept a little in from the bubble's edge)
    if (side == 1) p = { -p.x, -p.y, p.z };
    const float l = p.length();
    return l > 1.0f ? p * (1.0f / l) : p;
}
