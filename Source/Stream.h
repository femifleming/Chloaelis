#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>

// The built-in recording, streamed. A background thread decodes the compressed file, loops
// it (with an equal-power crossfade across the seam) and resamples it to the host rate into
// a lock-free FIFO; the audio thread only copies out of that FIFO, so it never touches the
// decoder, never allocates and never waits.
namespace chl
{
    struct LoopStream : public juce::TimeSliceClient
    {
        // file positions in samples (at the file's own rate)
        void setup (std::unique_ptr<juce::AudioFormatReader> r, double startSec, double endSec, double fadeSec, double firstSec, float trimDb = 0.0f)
        {
            trim = juce::Decibels::decibelsToGain (trimDb);
            reader = std::move (r);
            if (reader == nullptr) return;
            const double fsr = reader->sampleRate;
            const auto len = reader->lengthInSamples - 4096;          // skip the encoder's padding at the end
            loopEnd   = std::clamp<juce::int64> ((juce::int64) (endSec < 0 ? (double) len : endSec * fsr), 1, len);
            xfade     = (juce::int64) (fadeSec * fsr);
            loopStart = std::clamp<juce::int64> ((juce::int64) (startSec * fsr), xfade, loopEnd - xfade - 1);
            first     = std::clamp<juce::int64> ((juce::int64) (firstSec * fsr), 0, loopEnd - xfade - 1);
        }

        // not on the audio thread, and only while the stream is off the background thread
        void prepare (double hostRate)
        {
            if (reader == nullptr) return;
            ratio = reader->sampleRate / hostRate;
            const int cap = (int) (hostRate * 1.5) + 8192;
            ring.setSize (2, cap);
            ring.clear();
            fifo.setTotalSize (cap);
            fifo.reset();
            stage.setSize (2, stageCap);
            raw1.setSize (2, rawCap); raw2.setSize (2, rawCap);
            out.setSize (2, outCap);
            staged = 0;
            pos = first;
            for (auto& ip : interp) ip.reset();
        }

        // audio thread: n frames into l / r (added, times gain). Returns false on underrun.
        bool pullAdd (float* l, float* r, int n, float gain0, float gain1)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (n, s1, n1, s2, n2);
            const float step = (gain1 - gain0) * trim / (float) juce::jmax (1, n);
            float g = gain0 * trim;
            auto copy = [&] (int start, int count, int offset)
            {
                const float* a = ring.getReadPointer (0, start);
                const float* b = ring.getReadPointer (1, start);
                for (int i = 0; i < count; ++i, g += step) { l[offset + i] += a[i] * g; r[offset + i] += b[i] * g; }
            };
            if (n1 > 0) copy (s1, n1, 0);
            if (n2 > 0) copy (s2, n2, n1);
            fifo.finishedRead (n1 + n2);
            return n1 + n2 == n;
        }

        int useTimeSlice() override
        {
            if (reader == nullptr) return 500;
            const int freeSpace = fifo.getFreeSpace();
            if (freeSpace < 2048) return 15;
            const int toMake = juce::jmin (freeSpace, outCap);

            // enough file-rate audio staged for this many output samples
            const int needIn = (int) std::ceil ((double) toMake * ratio) + 16;
            while (staged < needIn)
            {
                const int k = juce::jmin (rawCap, stageCap - staged);
                if (k <= 0) break;
                readLooped (stage.getWritePointer (0, staged), stage.getWritePointer (1, staged), k);
                staged += k;
            }
            const int used = interp[0].process (ratio, stage.getReadPointer (0), out.getWritePointer (0), toMake, staged, 0);
            interp[1].process (ratio, stage.getReadPointer (1), out.getWritePointer (1), toMake, staged, 0);
            const int left = staged - used;
            for (int c = 0; c < 2; ++c)
                std::memmove (stage.getWritePointer (c), stage.getReadPointer (c, used), sizeof (float) * (size_t) left);
            staged = left;

            int s1, n1, s2, n2;
            fifo.prepareToWrite (toMake, s1, n1, s2, n2);
            for (int c = 0; c < 2; ++c)
            {
                if (n1 > 0) ring.copyFrom (c, s1, out, c, 0, n1);
                if (n2 > 0) ring.copyFrom (c, s2, out, c, n1, n2);
            }
            fifo.finishedWrite (n1 + n2);
            return fifo.getFreeSpace() > 2048 ? 0 : 15;
        }

    private:
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::int64 loopStart = 0, loopEnd = 0, xfade = 0, first = 0, pos = 0;
        double ratio = 1.0;
        float trim = 1.0f;     // level match between the recordings
        juce::LagrangeInterpolator interp[2];
        static constexpr int stageCap = 32768, rawCap = 8192, outCap = 8192;
        juce::AudioBuffer<float> stage, raw1, raw2, out, ring;
        int staged = 0;
        juce::AbstractFifo fifo { 16 };

        void readRaw (juce::AudioBuffer<float>& b, juce::int64 at, int k)
        {
            reader->read (&b, 0, k, at, true, true);
        }

        // k file-rate samples from the loop: plain playback, then across the last `xfade`
        // samples the end fades out while the run-up to the loop start fades in
        void readLooped (float* l, float* r, int k)
        {
            int done = 0;
            while (done < k)
            {
                const auto fadeStart = loopEnd - xfade;
                if (pos < fadeStart)
                {
                    const int m = (int) std::min<juce::int64> (k - done, std::min<juce::int64> (rawCap, fadeStart - pos));
                    readRaw (raw1, pos, m);
                    std::memcpy (l + done, raw1.getReadPointer (0), sizeof (float) * (size_t) m);
                    std::memcpy (r + done, raw1.getReadPointer (1), sizeof (float) * (size_t) m);
                    pos += m; done += m;
                }
                else
                {
                    const int m = (int) std::min<juce::int64> (k - done, std::min<juce::int64> (rawCap, loopEnd - pos));
                    readRaw (raw1, pos, m);
                    readRaw (raw2, loopStart - (loopEnd - pos), m);
                    for (int i = 0; i < m; ++i)
                    {
                        const double t = (double) (pos + i - fadeStart) / (double) std::max<juce::int64> (1, xfade);
                        const float a = (float) std::cos (t * juce::MathConstants<double>::halfPi);
                        const float b = (float) std::sin (t * juce::MathConstants<double>::halfPi);
                        l[done + i] = raw1.getSample (0, i) * a + raw2.getSample (0, i) * b;
                        r[done + i] = raw1.getSample (1, i) * a + raw2.getSample (1, i) * b;
                    }
                    pos += m; done += m;
                    if (pos >= loopEnd) pos = loopStart;
                }
            }
        }
    };
}
