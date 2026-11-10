#pragma once
#include "Ambi.h"

namespace fp
{
// (the desktop app listens at 3rd order: the first 16 channels, whatever order goes out)
constexpr int kMonCh = 16;
namespace ambi
{
// Listening in the desktop app (the plug-ins don't decode: in a DAW the 16 channels go to a
// decoder of your choice). The app has no DAW round it, so it can decode for headphones or two
// speakers itself. These are the decoders tested in Mellyloon's first version.
//==============================================================================
// Stereo: the scene heard by two loudspeakers in front of you. Virtual directions all round,
// each panned by how far left or right it is (constant power), the ones behind a little quieter.
class StereoDecoder
{
public:
    StereoDecoder()
    {
        const auto dirs = fibonacci (40, true);
        const auto D = samplingDecoder (dirs);
        for (auto& r : m) r.fill (0.0f);
        for (size_t k = 0; k < dirs.size(); ++k)
        {
            // how far left or right, pushed out a little so the sides land in the speakers
            const float y = std::sin (std::clamp (1.5f * std::asin (std::clamp (dirs[k].y, -1.0f, 1.0f)), -1.5707963f, 1.5707963f));
            const float back = dirs[k].x < 0.0f ? 1.0f + 0.3f * dirs[k].x : 1.0f;
            const float gl = std::sqrt (0.5f * (1.0f + y)) * back, gr = std::sqrt (0.5f * (1.0f - y)) * back;
            for (int n = 0; n < kMonCh; ++n) { m[0][(size_t) n] += gl * D[k][(size_t) n]; m[1][(size_t) n] += gr * D[k][(size_t) n]; }
        }
        // a source straight ahead comes out of both speakers at -3 dB
        const auto f = sh ({ 1, 0, 0 });
        float l = 0.0f;
        for (int n = 0; n < kMonCh; ++n) l += m[0][(size_t) n] * f[(size_t) n];
        const float k = 0.70710678f / std::max (1.0e-6f, l);
        for (auto& r : m) for (auto& v : r) v *= k;
    }

    void process (const float* const* in, float* L, float* R, int n) const noexcept
    {
        std::fill (L, L + n, 0.0f);
        std::fill (R, R + n, 0.0f);
        for (int c = 0; c < kMonCh; ++c)
        {
            const float gl = m[0][(size_t) c], gr = m[1][(size_t) c];
            for (int i = 0; i < n; ++i) { L[i] += gl * in[c][i]; R[i] += gr * in[c][i]; }
        }
    }
    std::array<Coefs, 2> m;
};

//==============================================================================
// Binaural, without measured HRTFs: 40 virtual loudspeakers round a spherical head, each one
// rendered to the two ears with Brown and Duda's structural model (1998): the time difference
// round a sphere (Woodworth), the head's shadow as a one-pole, one-zero shelf that depends on the
// angle to each ear, and the pinna as a few short echoes whose delays change with elevation.
// The speakers' impulse responses are folded into one short filter per ambisonic channel and ear
// (16 x 2 filters of 'taps' samples), run as a direct convolution a block at a time.
class BinauralDecoder
{
public:
    static constexpr int taps = 128;

    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        const auto dirs = fibonacci (20, true);
        const auto D = samplingDecoder (dirs);
        for (auto& e : h) for (auto& c : e) c.assign ((size_t) taps, 0.0f);
        std::vector<float> ir ((size_t) taps);
        for (int ear = 0; ear < 2; ++ear)
            for (size_t k = 0; k < dirs.size(); ++k)
            {
                speakerToEar (dirs[k], ear, ir);
                for (int c = 0; c < kMonCh; ++c)
                    for (int t = 0; t < taps; ++t) h[ear][(size_t) c][(size_t) t] += D[k][(size_t) c] * ir[(size_t) t];
            }
        // loudness: a source straight ahead reaches each ear at -6 dB (a stereo pair of grains, which
        // reaches both ears, then comes out about as loud as it went in, like the stereo decoder)
        {
            const auto f = sh ({ 1, 0, 0 });
            double e = 0.0;
            for (int t = 0; t < taps; ++t)
            {
                double s = 0.0;
                for (int c = 0; c < kMonCh; ++c) s += h[0][(size_t) c][(size_t) t] * f[(size_t) c];
                e += s * s;
            }
            const float k = (float) (0.5 / std::sqrt (std::max (1.0e-12, e)));
            for (auto& ear : h) for (auto& c : ear) for (auto& v : c) v *= k;
        }
        hist.assign ((size_t) kMonCh, std::vector<float> ((size_t) (taps - 1 + std::max (1, maxBlock)), 0.0f));
    }

    // in: 16 channels; L, R: the two ears (overwritten)
    void process (const float* const* in, float* L, float* R, int n) noexcept
    {
        if (n <= 0) return;
        if ((int) hist[0].size() < taps - 1 + n)
            for (auto& hh : hist) hh.resize ((size_t) (taps - 1 + n), 0.0f);
        std::fill (L, L + n, 0.0f);
        std::fill (R, R + n, 0.0f);
        for (int c = 0; c < kMonCh; ++c)
        {
            float* x = hist[(size_t) c].data();
            std::copy (in[c], in[c] + n, x + taps - 1);
            const float* hl = h[0][(size_t) c].data();
            const float* hr = h[1][(size_t) c].data();
            for (int t = 0; t < taps; ++t)
            {
                const float* src = x + taps - 1 - t;
                const float a = hl[t], b = hr[t];
                if (a != 0.0f) for (int i = 0; i < n; ++i) L[i] += a * src[i];
                if (b != 0.0f) for (int i = 0; i < n; ++i) R[i] += b * src[i];
            }
            std::move (x + n, x + n + taps - 1, x);
        }
    }

    void reset() { for (auto& hh : hist) std::fill (hh.begin(), hh.end(), 0.0f); }

    // one virtual speaker to one ear (0 left, 1 right), as a short impulse response
    void speakerToEar (Vec3 d, int ear, std::vector<float>& ir) const
    {
        std::fill (ir.begin(), ir.end(), 0.0f);
        constexpr double a = 0.0875, c = 343.0;              // head radius (m), speed of sound
        const Vec3 earAxis { 0.0f, ear == 0 ? 1.0f : -1.0f, 0.0f };
        const double cosT = std::clamp ((double) d.dot (earAxis), -1.0, 1.0);
        const double theta = std::acos (cosT);                // 0: straight at this ear, pi: the far side
        // Woodworth: the far ear hears it later, round the head
        const double itd = theta < kPi * 0.5 ? a / c * (1.0 - std::cos (theta)) : a / c * (theta - kPi * 0.5 + 1.0);
        // the head's shadow (Brown and Duda): alpha 2 at the ear (a +6 dB high shelf), 0.1 behind the head
        const double thetaMin = 150.0 * kPi / 180.0;
        const double alpha = 1.05 + 0.95 * std::cos (theta / thetaMin * kPi);
        const double w0 = c / a;
        // bilinear transform of (1 + alpha s / 2w0) / (1 + s / 2w0)
        const double K = 2.0 * sr, tau = 1.0 / (2.0 * w0);
        const double b0 = 1.0 + alpha * tau * K, b1 = 1.0 - alpha * tau * K, a0 = 1.0 + tau * K, a1 = 1.0 - tau * K;
        // the pinna: echoes a few samples (at 44.1 kHz) behind, nearer together for sounds from above
        const Vec3 up { 0, 0, 1 };
        const double el = std::asin (std::clamp ((double) d.dot (up), -1.0, 1.0));
        const double frontness = std::clamp ((double) d.x, -1.0, 1.0);
        const double azEar = std::acos (std::clamp (frontness, -1.0, 1.0));   // 0 ahead .. pi behind
        static const double rho[5] = { 0.5, -1.0, 0.5, -0.25, 0.25 };
        static const double A[5] = { 1, 5, 5, 5, 5 }, B[5] = { 2, 4, 7, 11, 13 }, Dk[5] = { 1.0, 0.5, 0.5, 0.5, 0.5 };
        const double toSr = sr / 44100.0;
        // impulse through: delay (itd, fractional), + pinna echoes, then the shelf
        std::vector<double> x ((size_t) taps, 0.0);
        const double base = 2.0;   // a couple of samples so the fractional delays have room
        auto put = [&] (double at, double g)
        {
            // a short windowed sinc for the fractional position
            const int c0 = (int) std::floor (at);
            for (int k = -3; k <= 4; ++k)
            {
                const int i = c0 + k;
                if (i < 0 || i >= taps) continue;
                const double u = (double) i - at;
                const double s = std::abs (u) < 1.0e-9 ? 1.0 : std::sin (kPi * u) / (kPi * u);
                const double win = 0.5 + 0.5 * std::cos (kPi * u / 4.5);
                x[(size_t) i] += g * s * win;
            }
        };
        const double t0 = base + itd * sr;
        put (t0, 1.0);
        for (int k = 0; k < 5; ++k)
        {
            const double tk = (A[k] * std::cos (azEar * 0.5) * std::sin (Dk[k] * (kPi * 0.5 - el)) + B[k]) * toSr;
            put (t0 + std::max (1.0, tk), 0.25 * rho[k]);   // kept gentle: colour, not combing
        }
        double z = 0.0;
        for (int i = 0; i < taps; ++i)
        {
            // direct form II transposed, one pole one zero
            const double in = x[(size_t) i];
            const double y = (b0 / a0) * in + z;
            z = (b1 / a0) * in - (a1 / a0) * y;
            ir[(size_t) i] = (float) y;
        }
        // fade the tail so the truncation is smooth
        for (int i = taps - 16; i < taps; ++i) ir[(size_t) i] *= 0.5f + 0.5f * std::cos ((float) kPi * (float) (i - (taps - 16)) / 16.0f);
    }

    std::array<std::array<std::vector<float>, kMonCh>, 2> h;
private:
    double sr = 48000.0;
    std::vector<std::vector<float>> hist;
};

} // namespace ambi
} // namespace fp
