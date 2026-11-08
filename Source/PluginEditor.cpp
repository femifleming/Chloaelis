#include "PluginEditor.h"
#include <complex>
#include <BinaryData.h>

using namespace juce;

// profiling counters (read by the host test's benchmark)
int chloaelisDbgRenders = 0, chloaelisDbgBlurs = 0;
int chloaelisDbgFrames = 0;
static int diffTextPass = 0;   // 0 = labels paint normally, 1 = labels hidden, 2 = labels only (see DiffLabel)

// Look: a dark, glowing microscope-HUD in the spirit of the game Splice. A deep field with
// drifting bokeh and dust, film grain and vignette; every control is a soft dark capsule
// "cell" in a network of dashed links that follows the signal path; knobs are segmented HUD
// dials. A tilt-shift depth of field follows the mouse: a band of sharp focus at the cursor's
// height, everything above and below it melting into blur, with the layers in parallax.
// The field is also seen in perspective: the camera leans toward the cursor's side, so that
// side comes up close and large while the far side recedes and softens, and the cells nearest
// the cursor rise out of the sheet toward you (and draw over the ones behind).
// The palette rests cool and grey and warms toward copper / gold with the input level (a
// level meter made of colour, per side for the accents), and there are no level
// meters: each side's cells glow with its output level (accent > warm white > red).
namespace
{
    const Colour white   (0xffffffff);
    const Colour cell    (0xff10161b);

    struct Palette { Colour field0, field1, accL, accR, mote, haze, iri; };
    // at rest: cool slate greys with a faint blue-teal cast
    const Palette cool   { Colour (0xff0a0f13), Colour (0xff35434c), Colour (0xffb4c8d2), Colour (0xffc6d2d8), Colour (0xffe4ecf0), Colour (0x33607884), Colour (0xffc0c8e0) };
    const Palette copper { Colour (0xff140a05), Colour (0xff6e3a17), Colour (0xffffa66a), Colour (0xffffd56e), Colour (0xffffe3b8), Colour (0x33b8682a), Colour (0xffff8fb3) };
    Palette pal = cool;

    // warmth 0..1 per side, from the input level: the field follows the warmer side
    void updatePalette (float warmL, float warmR)
    {
        const float kf = jmax (warmL, warmR);
        auto mix = [] (Colour a, Colour b, float k) { return a.interpolatedWith (b, k); };
        pal = { mix (cool.field0, copper.field0, kf), mix (cool.field1, copper.field1, kf), mix (cool.accL, copper.accL, warmL),
                mix (cool.accR, copper.accR, warmR), mix (cool.mote, copper.mote, kf), mix (cool.haze, copper.haze, kf), mix (cool.iri, copper.iri, kf) };
    }
    Colour acc (int side) { return side == 0 ? pal.accL : side == 1 ? pal.accR : pal.accL.interpolatedWith (pal.accR, 0.5f); }

    // level -> colour: the side's accent when it's moving, warm white when it's hot, red at the top
    float levelDb (float meanAbs) { return Decibels::gainToDecibels (meanAbs * 1.57f, -90.0f); }
    Colour heat (int side, float db)
    {
        auto c = acc (side);
        c = c.interpolatedWith (Colour (0xfffff0dc), jlimit (0.0f, 1.0f, (db + 14.0f) / 10.0f));
        return c.interpolatedWith (Colour (0xffff4a3a), jlimit (0.0f, 1.0f, (db + 3.5f) / 3.5f));
    }

    // Nunito (a bold cut; SIL Open Font License), embedded so the UI looks the same everywhere
    Typeface::Ptr roundedBold()
    {
        static Typeface::Ptr face = Typeface::createSystemTypefaceFor (BinaryData::NunitoBold_ttf, BinaryData::NunitoBold_ttfSize);
        return face;
    }
    Font hud (float h, bool = false)   { return Font (FontOptions (roundedBold()).withHeight (h * 1.12f).withKerningFactor (0.08f)); }   // all Nunito Bold (its digits run small: scaled up to the old size)

    Point<float> polar (Point<float> c, float r, float a) { return { c.x + r * std::sin (a), c.y - r * std::cos (a) }; }
    double now() { return Time::getMillisecondCounterHiRes() * 0.001; }

    void txt (Graphics& g, const String& s, Rectangle<float> r, const Font& f, Colour c, Justification j = Justification::centred)
    {
        g.setFont (f); g.setColour (c); g.drawText (s, r, j, false);
    }

    // glowing stroke: halo + core
    void glow (Graphics& g, const Path& p, Colour c, float w)
    {
        g.setColour (c.withAlpha (0.10f * c.getFloatAlpha())); g.strokePath (p, PathStrokeType (w * 5.0f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (c.withAlpha (0.25f * c.getFloatAlpha())); g.strokePath (p, PathStrokeType (w * 2.4f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (c);                                       g.strokePath (p, PathStrokeType (w, PathStrokeType::curved, PathStrokeType::rounded));
    }
    void glowCircle (Graphics& g, Point<float> c, float r, Colour col, float w)
    {
        Path p; p.addEllipse (c.x - r, c.y - r, r * 2, r * 2);
        glow (g, p, col, w);
    }
    void dashed (Graphics& g, Point<float> a, Point<float> b, Colour c, float w = 1.0f)
    {
        const float dl[] = { 4.0f, 4.0f };
        Path p; p.startNewSubPath (a); p.lineTo (b);
        Path d; PathStrokeType (w).createDashedStroke (d, p, dl, 2);
        g.setColour (c); g.fillPath (d);
    }

    // a soft dark capsule cell with a fuzzy edge
    void capsule (Graphics& g, Rectangle<float> r, float angle, Colour rim, float rimAlpha, float fill = 1.0f)
    {
        Graphics::ScopedSaveState ss (g);
        g.addTransform (AffineTransform::rotation (angle, r.getCentreX(), r.getCentreY()));
        const float rad = jmin (r.getWidth(), r.getHeight()) * 0.5f;
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (Colours::black.withAlpha (0.10f * fill));
            g.fillRoundedRectangle (r.expanded ((float) i * 1.6f).translated (0, 2.0f), rad + (float) i * 1.6f);
        }
        g.setGradientFill (ColourGradient (cell.brighter (0.25f).withAlpha (fill), r.getCentreX(), r.getY(), cell.withAlpha (fill), r.getCentreX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, rad);
        if (rimAlpha > 0.01f)
        {
            Path p; p.addRoundedRectangle (r.expanded (1.5f), rad + 1.5f);
            glow (g, p, rim.withAlpha (rimAlpha), 1.3f);
        }
        g.setColour (Colours::white.withAlpha (0.06f * fill));
        g.fillRoundedRectangle (r.reduced (rad * 0.4f, 2.0f).withHeight (r.getHeight() * 0.3f), rad * 0.4f);
    }
    // A smoky glass capsule: lighter at the top, a soft specular highlight that slides as the
    // camera leans (view = the lean in the cell's own frame, in px), a faint bounce of the
    // side's accent along the bottom edge, a fine bright rim and a soft shadow on the field.
    void glassCapsule (Graphics& g, Rectangle<float> r, float angle, Colour ac, float glowAmt, Point<float> view)
    {
        Graphics::ScopedSaveState ss (g);
        const auto c = r.getCentre();
        g.addTransform (AffineTransform::rotation (angle, c.x, c.y));
        const float rad = jmin (r.getWidth(), r.getHeight()) * 0.5f;
        for (int i = 4; i >= 1; --i)
        {
            g.setColour (Colours::black.withAlpha (0.085f));
            g.fillRoundedRectangle (r.expanded ((float) i * 2.0f).translated (3.0f, 8.0f), rad + (float) i * 2.0f);
        }
        if (glowAmt > 0.01f)
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (ac.withAlpha (0.08f * glowAmt * (float) (4 - i) / 3.0f));
                g.fillRoundedRectangle (r.expanded ((float) i * 2.5f), rad + (float) i * 2.5f);
            }
        // body
        {
            ColourGradient bg (Colour (48, 62, 70).withAlpha (0.92f), c.x, r.getY(), Colour (10, 15, 18).withAlpha (0.96f), c.x, r.getBottom(), false);
            bg.addColour (0.45, Colour (14, 20, 24).withAlpha (0.94f));
            g.setGradientFill (bg);
            g.fillRoundedRectangle (r, rad);
        }
        {
            Graphics::ScopedSaveState clip (g);
            Path body; body.addRoundedRectangle (r, rad);
            g.reduceClipRegion (body);
            // accent bounce light along the bottom
            g.setGradientFill (ColourGradient (ac.withAlpha (0.0f), c.x, r.getBottom() - 16.0f, ac.withAlpha (0.35f), c.x, r.getBottom(), false));
            g.fillRect (r.withTop (r.getBottom() - 16.0f));
            // specular highlight, sliding against the lean
            const Point<float> hc (c.x - r.getWidth() * 0.22f - view.x * 2.2f, c.y - r.getHeight() * 0.26f - view.y * 1.4f);
            const float hw = r.getWidth() * 0.16f, hh = r.getHeight() * 0.2f;
            Graphics::ScopedSaveState hs (g);
            g.addTransform (AffineTransform::scale (1.0f, hh / hw, hc.x, hc.y).rotated (-0.15f, hc.x, hc.y));
            g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.24f), hc, Colours::white.withAlpha (0.0f), hc.translated (hw * 1.3f, 0), true));
            g.fillEllipse (hc.x - hw * 1.3f, hc.y - hw * 1.3f, hw * 2.6f, hw * 2.6f);
        }
        g.setColour (Colours::white.withAlpha (0.45f).interpolatedWith (ac, 0.6f * glowAmt));
        g.drawRoundedRectangle (r, rad, 0.9f);
        g.setColour (Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (r.reduced (2.0f), rad - 2.0f, 1.0f);
    }

    // A glass vial: the parameter's cell as a translucent capsule filled with lit fluid up to
    // its value. Cylinder shading across the width (bright rims, darker core) makes it read as
    // round; the fluid's surface is a small lit ellipse, as if seen from a little above; a long
    // specular streak slides with the camera lean (view). mod >= 0 draws the modulated level as
    // a ghost line; pulse (0..1) brightens the fluid (an LFO breathing in it).
    struct VialMotion { float tilt = 0, phase = 0, amp = 0, time = 0; };
    void glassVial (Graphics& g, Rectangle<float> r, float angle, Colour ac, float value, float mod,
                    float pulse, bool hover, Point<float> view, const VialMotion& mo = {})
    {
        Graphics::ScopedSaveState ss (g);
        const auto c = r.getCentre();
        g.addTransform (AffineTransform::rotation (angle, c.x, c.y));
        const float rad = r.getWidth() * 0.5f;
        const float x0 = r.getX(), x1 = r.getRight();
        for (int i = 4; i >= 1; --i)
        {
            g.setColour (Colours::black.withAlpha (0.08f));
            g.fillRoundedRectangle (r.expanded ((float) i * 2.0f).translated (3.0f, 8.0f), rad + (float) i * 2.0f);
        }
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (ac.withAlpha ((0.035f + 0.05f * value + (hover ? 0.05f : 0.0f)) * (float) (4 - i) / 3.0f));
            g.fillRoundedRectangle (r.expanded ((float) i * 2.6f), rad + (float) i * 2.6f);
        }
        Path body; body.addRoundedRectangle (r, rad);
        {
            Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (body);
            {
                const auto edge = Colour (110, 124, 132).interpolatedWith (ac, 0.35f);
                ColourGradient gg (edge.withAlpha (0.42f), x0, c.y, edge.withAlpha (0.42f), x1, c.y, false);
                gg.addColour (0.16, Colour (40, 50, 56).withAlpha (0.42f));
                gg.addColour (0.5,  Colour (10, 14, 17).withAlpha (0.5f));
                gg.addColour (0.84, Colour (32, 42, 48).withAlpha (0.44f));
                g.setGradientFill (gg);
                g.fillRect (r);
            }
            const float inner = r.getHeight() - 6.0f;
            const float level = r.getBottom() - 3.0f - jlimit (0.0f, 1.0f, value) * inner;
            const float eh = jmax (3.0f, r.getWidth() * 0.16f);
            if (value > 0.001f)
            {
                const auto lit = ac.withMultipliedBrightness (1.0f + 0.35f * pulse);
                // the moving surface: a slosh (tilt), two travelling waves, and the top face
                // of the liquid as a thin ellipse riding on it
                const float w = r.getWidth();
                constexpr int N = 14;
                float yF[N + 1], yB[N + 1], xs[N + 1];
                for (int i = 0; i <= N; ++i)
                {
                    const float u = (float) i / (float) N, x = x0 + u * w;
                    const float yc = level + mo.tilt * (x - c.x)
                                     + mo.amp * std::sin (mo.phase + u * 5.4f)
                                     + mo.amp * 0.45f * std::sin (mo.phase * 1.63f + 1.3f - u * 8.1f);
                    const float half = eh * 0.5f * std::sqrt (jmax (0.0f, 1.0f - (2.0f * u - 1.0f) * (2.0f * u - 1.0f)));
                    xs[i] = x; yF[i] = yc + half; yB[i] = yc - half;
                }
                Path fluid;
                fluid.startNewSubPath (x0, r.getBottom() + 2.0f);
                for (int i = 0; i <= N; ++i) fluid.lineTo (xs[i], yF[i]);
                fluid.lineTo (x1, r.getBottom() + 2.0f);
                fluid.closeSubPath();
                ColourGradient fg (lit.withAlpha (0.95f), x0, c.y, lit.withAlpha (0.95f), x1, c.y, false);
                fg.addColour (0.2, lit.withMultipliedBrightness (0.72f).withAlpha (0.9f));
                fg.addColour (0.5, lit.withMultipliedBrightness (0.5f).withAlpha (0.88f));
                fg.addColour (0.8, lit.withMultipliedBrightness (0.66f).withAlpha (0.9f));
                g.setGradientFill (fg);
                g.fillPath (fluid);
                g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.0f), c.x, level, Colours::black.withAlpha (0.28f), c.x, r.getBottom(), false));
                g.fillPath (fluid);
                // bubbles rising through it
                const int nb = w > 30.0f ? 5 : 2;
                const float depth = r.getBottom() - level;
                for (int b = 0; b < nb; ++b)
                {
                    const float seed = (float) b * 1.618f;
                    const float sp = 0.10f + 0.07f * std::fmod (seed * 3.3f, 1.0f);
                    const float rise = std::fmod (mo.time * sp + std::fmod (seed, 1.0f), 1.0f);
                    const float by = r.getBottom() - 4.0f - rise * (depth + 2.0f);
                    if (by < level + 3.0f || depth < 8.0f) continue;
                    const float bx = c.x + w * (0.28f * std::sin (seed * 4.0f) + 0.06f * std::sin (mo.time * 2.3f + seed * 7.0f));
                    const float br = (w > 30.0f ? 1.3f : 0.8f) * (0.7f + 0.6f * std::fmod (seed * 1.7f, 1.0f));
                    g.setColour (Colours::white.withAlpha (0.18f + 0.25f * rise));
                    g.drawEllipse (bx - br, by - br, br * 2, br * 2, 0.7f);
                }
                // the top face, its far edge, and the bright near edge (meniscus)
                Path face, nearE, farE;
                face.startNewSubPath (xs[0], yF[0]);
                for (int i = 1; i <= N; ++i) face.lineTo (xs[i], yF[i]);
                for (int i = N; i >= 0; --i) face.lineTo (xs[i], yB[i]);
                face.closeSubPath();
                g.setColour (lit.interpolatedWith (Colours::white, 0.35f).withAlpha (0.55f));
                g.fillPath (face);
                nearE.startNewSubPath (xs[0], yF[0]); farE.startNewSubPath (xs[0], yB[0]);
                for (int i = 1; i <= N; ++i) { nearE.lineTo (xs[i], yF[i]); farE.lineTo (xs[i], yB[i]); }
                g.setColour (Colours::white.withAlpha (0.35f));
                g.strokePath (farE, PathStrokeType (0.8f, PathStrokeType::curved));
                g.setColour (Colours::white.withAlpha (hover ? 0.95f : 0.8f));
                g.strokePath (nearE, PathStrokeType (1.1f, PathStrokeType::curved));
            }
            if (mod >= 0.0f && std::abs (mod - value) > 0.004f)
            {
                const float ml = r.getBottom() - 3.0f - jlimit (0.0f, 1.0f, mod) * inner;
                g.setColour (ac.interpolatedWith (Colours::white, 0.5f).withAlpha (0.55f));
                g.drawEllipse (x0 + 2.5f, ml - eh * 0.5f, r.getWidth() - 5.0f, eh, 0.9f);
            }
            {
                const float sx = x0 + r.getWidth() * 0.26f - view.x * 1.6f;
                ColourGradient sg (Colours::white.withAlpha (0.0f), sx - r.getWidth() * 0.12f, c.y, Colours::white.withAlpha (0.0f), sx + r.getWidth() * 0.12f, c.y, false);
                sg.addColour (0.5, Colours::white.withAlpha (0.2f));
                g.setGradientFill (sg);
                g.fillRect (Rectangle<float> (sx - r.getWidth() * 0.12f, r.getY() + rad * 0.55f, r.getWidth() * 0.24f, r.getHeight() - rad * 1.1f));
                const Point<float> hc (x0 + r.getWidth() * 0.32f - view.x * 1.6f, r.getY() + rad * 0.62f - view.y * 1.2f);
                const float hr = r.getWidth() * 0.16f;
                g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.5f), hc, Colours::white.withAlpha (0.0f), hc.translated (hr, 0), true));
                g.fillEllipse (hc.x - hr, hc.y - hr, hr * 2, hr * 2);
            }
        }
        g.setColour (ac.withAlpha (0.22f + (hover ? 0.25f : 0.0f)));
        g.drawRoundedRectangle (r.expanded (1.0f), rad + 1.0f, 3.0f);
        g.setColour (ac.withAlpha (0.5f + (hover ? 0.3f : 0.0f)));
        g.drawRoundedRectangle (r.reduced (1.2f), rad - 1.2f, 1.8f);
        g.setColour (Colours::white.withAlpha (0.55f));
        g.drawRoundedRectangle (r, rad, 0.8f);
    }

    // A sliding collar: a dark glass capsule with a ring that slides along it. The ring is
    // drawn as a solid band wrapping the body (cylinder-shaded) with its top face as an
    // ellipse, so it reads as a real object around the capsule. The track below it is lit.
    // mod >= 0 shows where modulation has pushed it, as a ghost ring.
    void glassCollar (Graphics& g, Rectangle<float> r, float angle, Colour ac, float value, float mod, bool hover, Point<float> view)
    {
        Graphics::ScopedSaveState ss (g);
        const auto c = r.getCentre();
        g.addTransform (AffineTransform::rotation (angle, c.x, c.y));
        const float rad = r.getWidth() * 0.5f, w = r.getWidth();
        const float x0 = r.getX(), x1 = r.getRight();
        for (int i = 4; i >= 1; --i)
        {
            g.setColour (Colours::black.withAlpha (0.08f));
            g.fillRoundedRectangle (r.expanded ((float) i * 2.0f).translated (3.0f, 8.0f), rad + (float) i * 2.0f);
        }
        const float top = r.getY() + 32.0f, bot = r.getBottom() - 26.0f;   // clear of the symbol and the value
        auto yAt = [&] (float v) { return bot - jlimit (0.0f, 1.0f, v) * (bot - top); };
        const float y = yAt (value);
        Path body; body.addRoundedRectangle (r, rad);
        {
            Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (body);
            ColourGradient gg (Colour (64, 76, 84).withAlpha (0.6f), x0, c.y, Colour (64, 76, 84).withAlpha (0.6f), x1, c.y, false);
            gg.addColour (0.18, Colour (26, 33, 38).withAlpha (0.82f));
            gg.addColour (0.5,  Colour (9, 13, 16).withAlpha (0.9f));
            gg.addColour (0.82, Colour (22, 29, 34).withAlpha (0.84f));
            g.setGradientFill (gg);
            g.fillRect (r);
            // the track: dim all the way, lit from the bottom up to the collar
            g.setColour (Colours::white.withAlpha (0.14f));
            g.fillRoundedRectangle (c.x - 1.0f, top, 2.0f, bot - top, 1.0f);
            g.setGradientFill (ColourGradient (ac.withAlpha (0.25f), c.x, bot, ac.withAlpha (0.95f), c.x, y, false));
            g.fillRoundedRectangle (c.x - 1.5f, y, 3.0f, bot - y, 1.5f);
            g.setColour (ac.withAlpha (0.18f));
            g.fillRoundedRectangle (c.x - 5.0f, y, 10.0f, bot - y, 5.0f);
            // specular streak
            const float sx = x0 + w * 0.26f - view.x * 1.6f;
            ColourGradient sg (Colours::white.withAlpha (0.0f), sx - w * 0.12f, c.y, Colours::white.withAlpha (0.0f), sx + w * 0.12f, c.y, false);
            sg.addColour (0.5, Colours::white.withAlpha (0.16f));
            g.setGradientFill (sg);
            g.fillRect (Rectangle<float> (sx - w * 0.12f, r.getY() + rad * 0.55f, w * 0.24f, r.getHeight() - rad * 1.1f));
        }
        g.setColour (Colours::white.withAlpha (0.45f).interpolatedWith (ac, hover ? 0.6f : 0.2f));
        g.drawRoundedRectangle (r, rad, 0.9f);

        auto ring = [&] (float yy, float alpha, bool ghost)
        {
            const float rw = w + 9.0f, rh = 12.0f, fh = 6.0f;      // ring width, band height, top-face height
            const float rx = c.x - rw * 0.5f;
            if (ghost)
            {
                g.setColour (ac.interpolatedWith (Colours::white, 0.4f).withAlpha (0.5f * alpha));
                g.drawEllipse (rx, yy - fh * 0.5f, rw, fh, 0.9f);
                g.drawEllipse (rx, yy - fh * 0.5f + rh, rw, fh, 0.6f);
                return;
            }
            // band (the outer face of the ring), cylinder-shaded
            Path band;
            band.addRoundedRectangle (rx, yy, rw, rh, 2.0f);
            band.addCentredArc (c.x, yy + rh, rw * 0.5f, fh * 0.5f, 0.0f, MathConstants<float>::halfPi, MathConstants<float>::halfPi + MathConstants<float>::pi, true);
            ColourGradient bg (ac.withMultipliedBrightness (0.45f), rx, yy, ac.withMultipliedBrightness (0.45f), rx + rw, yy, false);
            bg.addColour (0.25, ac.withMultipliedBrightness (1.15f));
            bg.addColour (0.33, ac.interpolatedWith (Colours::white, 0.55f));
            bg.addColour (0.55, ac.withMultipliedBrightness (0.8f));
            g.setGradientFill (bg);
            g.fillRoundedRectangle (rx, yy, rw, rh, 2.0f);
            g.fillEllipse (rx, yy + rh - fh * 0.5f, rw, fh);
            // top face with the hole the capsule passes through
            g.setColour (ac.interpolatedWith (Colours::white, 0.45f));
            g.fillEllipse (rx, yy - fh * 0.5f, rw, fh);
            g.setColour (Colour (12, 16, 19));
            g.fillEllipse (c.x - w * 0.5f, yy - fh * 0.32f, w, fh * 0.64f);
            g.setColour (Colours::white.withAlpha (hover ? 0.95f : 0.75f));
            g.drawEllipse (rx, yy - fh * 0.5f, rw, fh, 0.8f);
            // glow
            g.setColour (ac.withAlpha (hover ? 0.3f : 0.18f));
            g.drawRoundedRectangle (rx - 2.0f, yy - fh * 0.5f - 2.0f, rw + 4.0f, rh + fh + 4.0f, 5.0f, 2.0f);
        };
        if (mod >= 0.0f && std::abs (mod - value) > 0.004f) ring (yAt (mod), 1.0f, true);
        ring (y - 6.0f, 1.0f, false);
    }

    // Each parameter is named by a Greek letter (its full name is in the tooltip)
    String symbolFor (const String& name)
    {
        static const std::pair<const char*, const char*> map[] = {
            { "Input", "\xce\xb3" },   // gamma: gain
            { "Cutoff", "\xcf\x89" },  // omega: cutoff frequency
            { "Reso", "\xce\xb6" },    // zeta: damping / resonance
            { "Mode", "\xce\xbc" },    // mu: morph between filter modes
            { "Drive", "\xce\xba" },   // kappa: the curve of the tube
            { "Time", "\xcf\x84" },    // tau: delay time
            { "Regen", "\xcf\x86" },   // phi: feedback
            { "Mix", "\xce\xb7" },     // eta: dry / wet ratio
            { "Rand", "\xce\xbe" },    // xi: chance of a random freeze
            { "Out", "\xce\xa9" },     // Omega: the end of the chain
            { "Speed", "\xce\xbd" },   // nu: how fast the attractor runs
            { "Chaos", "\xcf\x81" },   // rho: the Lorenz rho
            { "Shape", "\xce\xb2" },   // beta: the Lorenz beta
            { "Depth", "\xce\x94" },   // Delta: how far it moves the cutoffs
            { "Width", "\xcf\x88" },   // psi: poly spread
            { "Source", "\xcf\x83" },  // sigma: the built-in recording's level
            { "Feedback", "\xce\xa6" },// Phi: master feedback, out back into in
            { "Flock", "\xcf\x87" },   // chi: chaos for the poly taps (a boids flock moves their times)
        };
        for (auto& [n, sym] : map) if (name == n) return String::fromUTF8 (sym);
        return name;
    }
    Font greek (float h) { return Font (FontOptions().withName ("Helvetica Neue").withStyle ("Regular").withHeight (h)); }

    // short value text: no units, no prefixes (the full text shows while the control is hovered)
    String compactValue (const String& full)
    {
        auto t = full.trim();
        for (auto* cut : { " Hz", "Hz", " ms", "%" }) t = t.replace (cut, "");
        t = t.replace (String::fromUTF8 ("\xcf\x81 "), "").replace (String::fromUTF8 ("\xce\xb2 "), "");
        if (t.containsChar ('>')) t = t.upToFirstOccurrenceOf (" ", false, false).replace (">", String::fromUTF8 ("\xe2\x80\xba"));
        if (t == "0.0" || t == "-0.0") t = "0";
        else if (t.startsWith ("0.")) t = t.substring (1);
        return t;
    }

    void ghostCapsule (Graphics& g, Rectangle<float> r, float angle, float alpha)
    {
        Graphics::ScopedSaveState ss (g);
        g.addTransform (AffineTransform::rotation (angle, r.getCentreX(), r.getCentreY()));
        g.setColour (Colours::white.withAlpha (alpha));
        g.drawRoundedRectangle (r, jmin (r.getWidth(), r.getHeight()) * 0.5f, 1.0f);
    }

    // one face of a capsule button (lit rim + dot when on)
    void cellFace (Graphics& g, Rectangle<float> r, const String& label, bool on, bool over, Colour accent, float fontH = 10.0f)
    {
        capsule (g, r, 0.0f, on ? accent : white, on ? 0.95f : (over ? 0.5f : 0.22f));
        if (on)
        {
            const Point<float> d (r.getX() + r.getHeight() * 0.5f, r.getCentreY());
            g.setColour (accent.withAlpha (0.4f)); g.fillEllipse (d.x - 5, d.y - 5, 10, 10);
            g.setColour (accent); g.fillEllipse (d.x - 2.5f, d.y - 2.5f, 5, 5);
        }
        txt (g, label.toUpperCase(), r.withTrimmedLeft (on ? 8.0f : 0.0f), hud (fontH, true), on ? white : white.withAlpha (0.55f));
    }

    //==========================================================================
    // Depth of field without the per-frame cost. Each cell renders itself (and its controls)
    // once into a cached image, and only again when something inside it changes. The blur is
    // a gaussian-blurred copy at reduced resolution, made only when the cell's blur step changes.
    // Area-averaging downsample (premultiplied ARGB). JUCE's GL renderer samples textures with
    // plain bilinear and no mipmaps, so drawing a bigger image smaller on the GPU skips texels
    // and makes text and thin lines jagged. Instead, the cell is shrunk here to the exact size
    // it lands on screen and drawn 1:1.
    static Image areaDownsample (const Image& srcImg, int dw, int dh)
    {
        Image out (Image::ARGB, dw, dh, true, SoftwareImageType());
        out.setBackupEnabled (false);
        Image::BitmapData src (srcImg, Image::BitmapData::readOnly), dst (out, Image::BitmapData::writeOnly);
        const int sw = src.width, sh = src.height;
        struct Tap { int i; float w; };
        auto taps = [] (int s, int d)
        {
            std::vector<std::vector<Tap>> t ((size_t) d);
            const float r = (float) s / (float) d;
            for (int o = 0; o < d; ++o)
            {
                const float a = (float) o * r, b = a + r;
                for (int i = (int) a; i < jmin (s, (int) std::ceil (b)); ++i)
                {
                    const float w = jmin (b, (float) i + 1.0f) - jmax (a, (float) i);
                    if (w > 0.0f) t[(size_t) o].push_back ({ i, w / r });
                }
            }
            return t;
        };
        const auto tx = taps (sw, dw), ty = taps (sh, dh);
        std::vector<float> tmp ((size_t) dw * (size_t) sh * 4);
        for (int y = 0; y < sh; ++y)
        {
            const uint8* row = src.getLinePointer (y);
            float* o = &tmp[(size_t) y * (size_t) dw * 4];
            for (int x = 0; x < dw; ++x, o += 4)
            {
                float acc4[4] {};
                for (const auto& tp : tx[(size_t) x])
                {
                    const uint8* p = row + (size_t) tp.i * (size_t) src.pixelStride;
                    for (int c = 0; c < 4; ++c) acc4[c] += (float) p[c] * tp.w;
                }
                std::memcpy (o, acc4, sizeof (acc4));
            }
        }
        for (int y = 0; y < dh; ++y)
        {
            uint8* row = dst.getLinePointer (y);
            for (int x = 0; x < dw; ++x)
            {
                float acc4[4] {};
                for (const auto& tp : ty[(size_t) y])
                {
                    const float* p = &tmp[((size_t) tp.i * (size_t) dw + (size_t) x) * 4];
                    for (int c = 0; c < 4; ++c) acc4[c] += p[c] * tp.w;
                }
                uint8* d = row + (size_t) x * (size_t) dst.pixelStride;
                for (int c = 0; c < 4; ++c) d[c] = (uint8) jlimit (0, 255, (int) (acc4[c] + 0.5f));
            }
        }
        return out;
    }
    static int fitBudget = 6;   // area downsamples allowed per frame (reset in Content::tick)
    static double blurMsLeft = 4.0;  // CPU time for fresh blurs per frame (reset in Content::tick)

    // Gaussian blur (three box passes, zero outside the image) on premultiplied ARGB, in place.
    static void gaussianBlur (Image& img, float sigma)
    {
        if (sigma < 0.3f) return;
        Image::BitmapData bd (img, Image::BitmapData::readWrite);
        const int w = bd.width, h = bd.height;
        // box sizes approximating a gaussian (Kovesi)
        int boxes[3];
        {
            const float wIdeal = std::sqrt (12.0f * sigma * sigma / 3.0f + 1.0f);
            int wl = (int) std::floor (wIdeal); if (wl % 2 == 0) --wl;
            const int m = roundToInt ((12.0f * sigma * sigma - 3.0f * (float) (wl * wl) - 12.0f * (float) wl - 9.0f) / (-4.0f * (float) wl - 4.0f));
            for (int i = 0; i < 3; ++i) boxes[i] = (i < m ? wl : wl + 2);
        }
        std::vector<float> buf ((size_t) w * (size_t) h * 4), line ((size_t) jmax (w, h) * 4), out ((size_t) jmax (w, h) * 4);
        for (int y = 0; y < h; ++y)
        {
            const uint8* row = bd.getLinePointer (y);
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) buf[((size_t) y * (size_t) w + (size_t) x) * 4 + (size_t) c] = row[(size_t) x * (size_t) bd.pixelStride + (size_t) c];
        }
        auto pass = [&] (int n, int stride, int count, int lineStride, int rad)
        {
            const float inv = 1.0f / (float) (2 * rad + 1);
            for (int l = 0; l < count; ++l)
            {
                float* base = &buf[(size_t) l * (size_t) lineStride * 4];
                for (int i = 0; i < n; ++i) std::memcpy (&line[(size_t) i * 4], base + (size_t) i * (size_t) stride * 4, 16);
                float acc[4] {};
                for (int i = 0; i <= jmin (rad, n - 1); ++i) for (int c = 0; c < 4; ++c) acc[c] += line[(size_t) i * 4 + (size_t) c];
                for (int i = 0; i < n; ++i)
                {
                    for (int c = 0; c < 4; ++c) out[(size_t) i * 4 + (size_t) c] = acc[c] * inv;
                    const int add = i + rad + 1, sub = i - rad;
                    if (add < n) for (int c = 0; c < 4; ++c) acc[c] += line[(size_t) add * 4 + (size_t) c];
                    if (sub >= 0) for (int c = 0; c < 4; ++c) acc[c] -= line[(size_t) sub * 4 + (size_t) c];
                }
                for (int i = 0; i < n; ++i) std::memcpy (base + (size_t) i * (size_t) stride * 4, &out[(size_t) i * 4], 16);
            }
        };
        for (int b : boxes)
        {
            const int rad = (b - 1) / 2;
            if (rad < 1) continue;
            pass (w, 1, h, w, rad);   // rows
            pass (h, w, w, 1, rad);   // columns
        }
        for (int y = 0; y < h; ++y)
        {
            uint8* row = bd.getLinePointer (y);
            for (int x = 0; x < w; ++x)
            {
                const float* p = &buf[((size_t) y * (size_t) w + (size_t) x) * 4];
                uint8* d = row + (size_t) x * (size_t) bd.pixelStride;
                const int a = jlimit (0, 255, (int) (p[3] + 0.5f));
                for (int c = 0; c < 3; ++c) d[c] = (uint8) jlimit (0, a, (int) (p[c] + 0.5f));   // stay premultiplied
                d[3] = (uint8) a;
            }
        }
    }

    struct BlurCache : public CachedComponentImage
    {
        Component& owner;
        const float& radius;           // in component units
        Image sharp, small, fit, base;
        bool dirty = true, holdForMove = false, fitDirty = true, baseDirty = true;   // a move (transform change) doesn't change the pixels
        bool differenceText = false;              // child labels are difference-blended (see DiffLabel)
        float smallFor = -1.0f, smallPhys = -1.0f;
        int starved = 0;
        float zoom = 1.0f;             // the perspective scale in the owner's transform: the cache is
                                       // rendered at a fixed resolution with headroom for it, so
                                       // zooming in and out never forces a re-render

        BlurCache (Component& c, const float& r) : owner (c), radius (r) {}

        void paint (Graphics& g) override
        {
            const float phys = g.getInternalContext().getPhysicalPixelScaleFactor();
            const float scale = std::ceil (phys / jmax (0.1f, zoom) * 1.7f * 8.0f - 0.05f) / 8.0f;
            const auto cb = owner.getLocalBounds();
            const int iw = jmax (1, roundToInt ((float) cb.getWidth() * scale)), ih = jmax (1, roundToInt ((float) cb.getHeight() * scale));
            if (sharp.isNull() || sharp.getWidth() != iw || sharp.getHeight() != ih)
            {
                sharp = Image (Image::ARGB, iw, ih, true, SoftwareImageType());   // rendered on the CPU only when dirty
                sharp.setBackupEnabled (false);
                dirty = true;
            }
            if (dirty)
            {
                sharp.clear (sharp.getBounds());
                const auto toImg = AffineTransform::scale ((float) iw / (float) cb.getWidth(), (float) ih / (float) cb.getHeight());
                {
                    Graphics ig (sharp);
                    ig.addTransform (toImg);
                    diffTextPass = differenceText ? 1 : 0;
                    owner.paintEntireComponent (ig, true);
                    diffTextPass = 0;
                }
                if (differenceText) composeDifferenceText (toImg, iw, ih);
                ++chloaelisDbgRenders;
                dirty = false;
                fitDirty = true;
                baseDirty = true;
                smallFor = -1.0f;
            }

            // Blur strength in screen px, continuous from sharp (0) upward, then quantised in
            // ~4% steps: fine enough that a fade into focus reads as smooth, not as steps.
            const float qp = std::exp2 (std::round (std::log2 (jmax (0.05f, phys)) * 12.0f) / 12.0f);
            const float rr = jmax (0.0f, radius);
            const float sigma = 0.5f * rr * qp + 0.66f * jmin (1.0f, rr * 2.0f);
            const float sq = std::exp2 (std::round (std::log2 (sigma + 1.0f) * 16.0f) / 16.0f) - 1.0f;
            g.setColour (Colours::black.withAlpha (owner.getAlpha()));
            if (sq < 0.12f)
            {
                g.setImageResamplingQuality (Graphics::mediumResamplingQuality);
                // the size it actually covers on screen, in ~3% steps so small zoom wobbles reuse it
                const float q = std::exp2 (std::ceil (std::log2 (jmax (0.05f, phys)) * 24.0f) / 24.0f);
                const int fw = jmax (1, roundToInt ((float) cb.getWidth() * q)), fh = jmax (1, roundToInt ((float) cb.getHeight() * q));
                if (fw >= iw - 1)
                {
                    g.drawImageTransformed (sharp, AffineTransform::scale ((float) cb.getWidth() / (float) iw, (float) cb.getHeight() / (float) ih), false);
                    return;
                }
                if ((fit.getWidth() != fw || fit.getHeight() != fh || fitDirty) && fitBudget > 0)
                {
                    fit = areaDownsample (sharp, fw, fh);
                    fitDirty = false;
                    --fitBudget;
                }
                // over budget this frame: a stale fit within ~20% still beats a squeezed texture
                const float ratio = fit.isNull() ? 0.0f : (float) fit.getWidth() / (float) fw;
                const auto& img = (! fitDirty && ratio > 0.83f && ratio < 1.25f) ? fit : sharp;
                g.drawImageTransformed (img, AffineTransform::scale ((float) cb.getWidth() / (float) img.getWidth(), (float) cb.getHeight() / (float) img.getHeight()), false);
                return;
            }
            // A real gaussian at a resolution the blur can hide: area-averaged down (no skipped
            // pixels), blurred with sigma >= 1.5 texels, so the GPU's bilinear upscale stays smooth.
            const bool stale = std::abs (sq - smallFor) > 0.001f || std::abs (qp - smallPhys) > 0.001f || small.isNull();
            // a time budget per frame; a cell that missed its turn goes first next frame, so none lags
            if (stale && (blurMsLeft > 0.0 || small.isNull() || starved >= 2))
            {
                const double t0 = Time::getMillisecondCounterHiRes();
                // a screen-resolution copy to blur from (much smaller than the oversampled master)
                const int bw = jmax (2, roundToInt ((float) cb.getWidth() * qp)), bh = jmax (2, roundToInt ((float) cb.getHeight() * qp));
                if (base.getWidth() != bw || base.getHeight() != bh || baseDirty)
                {
                    base = areaDownsample (sharp, bw, bh);
                    baseDirty = false;
                }
                const float sigmaScreen = sq;
                const float d = jlimit (1.0f, 8.0f, sigmaScreen / 1.1f);      // screen px per blur texel
                const int sw = jmax (2, roundToInt ((float) bw / d)), sh = jmax (2, roundToInt ((float) bh / d));
                small = d > 1.01f ? areaDownsample (base, sw, sh) : base.createCopy();
                gaussianBlur (small, sigmaScreen / d);
                ++chloaelisDbgBlurs;
                blurMsLeft -= Time::getMillisecondCounterHiRes() - t0;
                smallFor = sq;
                smallPhys = qp;
                starved = 0;
            }
            else if (stale) ++starved;
            g.setImageResamplingQuality (Graphics::mediumResamplingQuality);
            g.drawImageTransformed (small, AffineTransform::scale ((float) cb.getWidth() / (float) small.getWidth(), (float) cb.getHeight() / (float) small.getHeight()), false);
        }
        void composeDifferenceText (const AffineTransform& toImg, int iw, int ih)
        {
            Image mask (Image::ARGB, iw, ih, true, SoftwareImageType());
            bool any = false;
            {
                Graphics mg (mask);
                mg.addTransform (toImg);
                diffTextPass = 2;
                for (auto* ch : owner.getChildren())
                    if (auto* l = dynamic_cast<Label*> (ch); l != nullptr && l->isVisible() && l->getText().isNotEmpty())
                    {
                        Graphics::ScopedSaveState ss (mg);
                        mg.setOrigin (l->getPosition());
                        l->paintEntireComponent (mg, true);
                        any = true;
                    }
                diffTextPass = 0;
            }
            if (! any) return;
            Image::BitmapData dst (sharp, Image::BitmapData::readWrite), src (mask, Image::BitmapData::readOnly);
            for (int y = 0; y < ih; ++y)
                for (int x = 0; x < iw; ++x)
                {
                    const auto* m = src.getPixelPointer (x, y);
                    const int a = m[3];                    // the mask's coverage x the label's own alpha
                    if (a == 0) continue;
                    auto* d = dst.getPixelPointer (x, y);  // premultiplied BGRA; transparent reads as the dark field
                    for (int c = 0; c < 3; ++c)
                        d[c] = (uint8) ((d[c] * (255 - a) + (255 - d[c]) * a + 127) / 255);
                    d[3] = (uint8) (d[3] + ((255 - d[3]) * a + 127) / 255);
                }
        }
        bool invalidateAll() override                    { if (! holdForMove) dirty = true; return true; }
        bool invalidate (const Rectangle<int>&) override { dirty = true; return true; }
        void releaseResources() override                 { sharp = {}; small = {}; fit = {}; base = {}; }
    };

    // tinting an alpha mask on the fly is slow in CoreGraphics, so keep a few ready-tinted copies
    struct TintedGlow
    {
        Colour colour { 0x00000000 };
        Image img;
        const Image& get (Colour c)
        {
            c = c.withAlpha (1.0f);
            if (img.isNull() || std::abs (c.getFloatRed() - colour.getFloatRed()) + std::abs (c.getFloatGreen() - colour.getFloatGreen())
                                + std::abs (c.getFloatBlue() - colour.getFloatBlue()) > 0.02f)
            {
                colour = c;
                img = Image (Image::ARGB, 128, 128, true, SoftwareImageType());
                Graphics g (img);
                g.setGradientFill (ColourGradient (c, 64, 64, c.withAlpha (0.0f), 128, 64, true));
                g.fillEllipse (0, 0, 128, 128);
            }
            return img;
        }
    };
    void drawGlow (Graphics& g, TintedGlow& cache, Point<float> c, float r, Colour col)
    {
        g.setOpacity (col.getFloatAlpha());
        g.drawImage (cache.get (col), Rectangle<float> (c.x - r, c.y - r, r * 2, r * 2), RectanglePlacement::stretchToFit, false);
    }

    //==========================================================================
    constexpr float W = 1200.0f, H = 820.0f;

    //==========================================================================
    // HUD dial: thin glowing ring, segmented value arc, target centre
    struct Knob : public Slider
    {
        int side = 0;
        bool big = false, mini = false;
        const std::atomic<float>* mod = nullptr;   // modulated position (normalised) -> a second dot
        const std::atomic<float>* lfo = nullptr;   // an LFO that lights the lens while the knob is up
        float lastMod = -1.0f, lastLfo = -9.0f;
        bool lastHover = false;
        // glass styles: 1 = a vial filled to the value (liquid that moves), 2 = a sliding collar
        int vialStyle = 0;
        Rectangle<float> vial;          // the capsule, in this knob's coords
        float vialAngle = 0.0f;
        const Point<float>* view = nullptr;
        const float* blur = nullptr;    // the cell's blur: blurred vials animate less often
        // the liquid: a damped slosh kicked by value changes and by the camera swinging
        float slosh = 0, sloshV = 0, lastPos = -1, lastViewX = 0;
        int animTick = 0;
        VialMotion mo;
        void animate()
        {
            const float pos = (float) valueToProportionOfLength (getValue());
            if (lastPos < 0) lastPos = pos;
            const float vx = view != nullptr ? view->x : 0.0f;
            const float kick = (pos - lastPos) * 3.0f + (vx - lastViewX) * 0.05f;
            lastPos = pos; lastViewX = vx;
            sloshV += -0.055f * slosh - 0.045f * sloshV + kick;
            slosh = jlimit (-0.45f, 0.45f, slosh + sloshV);
            mo.time += 1.0f / 60.0f;
            mo.tilt = slosh;
            mo.amp = (mini ? 0.35f : 0.6f) + (mini ? 3.0f : 7.0f) * std::abs (slosh);
            mo.phase += 0.07f + 0.6f * std::abs (sloshV);
        }
        Knob() : Slider (RotaryHorizontalVerticalDrag, NoTextBox)
        {
            setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            setMouseDragSensitivity (240);
            setMouseCursor (MouseCursor::PointingHandCursor);
        }
        void poll()
        {
            bool dirty = isMouseOverOrDragging() != lastHover;
            lastHover = isMouseOverOrDragging();
            if (mod != nullptr)
            {
                const float m = mod->load (std::memory_order_relaxed);
                if (std::abs (m - lastMod) > 0.002f) { lastMod = m; dirty = true; }
            }
            if (lfo != nullptr && getValue() > 0.0)
            {
                const float v = lfo->load (std::memory_order_relaxed);
                if (std::abs (v - lastLfo) > 0.02f) { lastLfo = v; dirty = true; }
            }
            if (big) dirty = true;
            if (vialStyle == 1)
            {
                animate();
                // repaint at 30 Hz when in focus; much less when the blur would hide it anyway
                const float b = blur != nullptr ? *blur : 0.0f;
                const int every = b < 1.5f ? (mini ? 4 : 2) : (b < 4.0f ? 6 : 12);
                if (++animTick % every == 0) dirty = true;
            }
            if (dirty) repaint();
        }
    };

    struct HudLnF : public LookAndFeel_V4
    {
        HudLnF()
        {
            setColour (TooltipWindow::backgroundColourId, Colour (0xf0081218));
            setColour (TooltipWindow::textColourId, white);
            setColour (TooltipWindow::outlineColourId, white.withAlpha (0.5f));
            setColour (BubbleComponent::backgroundColourId, Colour (0xf0081218));
            setColour (BubbleComponent::outlineColourId, white.withAlpha (0.5f));
        }

        // HUD dial: a segmented value arc, a dark lens with a glowing rim, a centre dot and a
        // pointer bead on the lens edge. Modulation shows as a second bead on the arc.
        void drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, Slider& s) override
        {
            auto* k = dynamic_cast<Knob*> (&s);
            const Colour ac = acc (k ? k->side : 0);
            const bool mini = k && k->mini, big = k && k->big;
            const auto c = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).getCentre();
            const float R = (float) jmin (w, h) * 0.5f - 5.0f;
            const float ang = a0 + pos * (a1 - a0);
            const bool hover = s.isMouseOverOrDragging();
            if (k != nullptr && k->vialStyle != 0)
            {
                const float m = k->mod != nullptr ? k->mod->load (std::memory_order_relaxed) : -1.0f;
                const auto view = k->view != nullptr ? *k->view : Point<float>();
                if (k->vialStyle == 2)
                    glassCollar (g, k->vial, k->vialAngle, ac, pos, m, hover, view);
                else
                {
                    const float pulse = (k->lfo != nullptr && s.getValue() > 0.0) ? jlimit (0.0f, 1.0f, k->lfo->load (std::memory_order_relaxed)) : 0.0f;
                    glassVial (g, k->vial, k->vialAngle, ac, pos, m, pulse, hover, view, k->mo);
                }
                return;
            }

            // one continuous value arc on the outside (fine line)
            const float rr = R - (mini ? 2.0f : 3.0f);
            const float lw = mini ? 1.5f : 2.2f;
            {
                Path tr; tr.addCentredArc (c.x, c.y, rr, rr, 0, a0, a1, true);
                g.setColour (white.withAlpha (0.13f));
                g.strokePath (tr, PathStrokeType (lw, PathStrokeType::curved, PathStrokeType::rounded));
                if (pos > 0.002f)
                {
                    Path v; v.addCentredArc (c.x, c.y, rr, rr, 0, a0, ang, true);
                    g.setColour (white.withAlpha (0.16f)); g.strokePath (v, PathStrokeType (lw * 2.3f, PathStrokeType::curved, PathStrokeType::rounded));
                    g.setColour (white.withAlpha (hover ? 1.0f : 0.92f)); g.strokePath (v, PathStrokeType (lw, PathStrokeType::curved, PathStrokeType::rounded));
                }
            }

            // dark lens with a glowing rim
            const float ri = R - (mini ? 6.0f : 8.5f);
            g.setColour (Colours::black.withAlpha (0.35f));
            g.fillEllipse (c.x - ri, c.y - ri, ri * 2, ri * 2);
            if (k != nullptr && k->lfo != nullptr && s.getValue() > 0.0)
            {
                const float v = k->lfo->load (std::memory_order_relaxed);
                g.setGradientFill (ColourGradient (ac.withAlpha (0.6f * v), c, ac.withAlpha (0.0f), c.translated (ri, 0), true));
                g.fillEllipse (c.x - ri, c.y - ri, ri * 2, ri * 2);
            }
            glowCircle (g, c, ri, (hover ? ac : white).withAlpha (0.95f), mini ? 0.8f : 1.0f);
            if (big)
            {
                Path dr;
                const float t = (float) now();
                for (int i = 0; i < 24; ++i)
                {
                    const float a = t * 0.3f + (float) i * MathConstants<float>::twoPi / 24.0f;
                    dr.addCentredArc (c.x, c.y, ri * 0.7f, ri * 0.7f, 0, a, a + 0.12f, true);
                }
                g.setColour (white.withAlpha (0.35f)); g.strokePath (dr, PathStrokeType (0.7f));
            }
            // centre: a half-opacity ring round the dot
            const float tc = mini ? 3.0f : 4.5f;
            g.setColour (white.withAlpha (0.5f)); g.drawEllipse (c.x - tc, c.y - tc, tc * 2, tc * 2, 1.0f);
            g.setColour (ac); g.fillEllipse (c.x - 1.8f, c.y - 1.8f, 3.6f, 3.6f);
            // pointer bead on the lens edge
            const auto d = polar (c, ri, ang);
            g.setColour (ac.withAlpha (0.35f)); g.fillEllipse (d.x - 4.0f, d.y - 4.0f, 8.0f, 8.0f);
            g.setColour (white); g.fillEllipse (d.x - 2.1f, d.y - 2.1f, 4.2f, 4.2f);

            if (k != nullptr && k->mod != nullptr)
            {
                const float m = k->mod->load (std::memory_order_relaxed);
                if (std::abs (m - pos) > 0.002f)
                {
                    const auto q = polar (c, rr, a0 + m * (a1 - a0));
                    g.setColour (ac.withAlpha (0.4f)); g.fillEllipse (q.x - 4, q.y - 4, 8, 8);
                    g.setColour (ac); g.fillEllipse (q.x - 2.0f, q.y - 2.0f, 4.0f, 4.0f);
                }
            }
        }

        Font getSliderPopupFont (Slider&) override { return hud (13.0f, true); }
        int getSliderPopupPlacement (Slider&) override { return BubbleComponent::above; }
        void drawBubble (Graphics& g, BubbleComponent&, const Point<float>&, const Rectangle<float>& body) override
        {
            g.setColour (Colour (0xe0081218)); g.fillRoundedRectangle (body, body.getHeight() * 0.5f);
            g.setColour (white.withAlpha (0.6f)); g.drawRoundedRectangle (body, body.getHeight() * 0.5f, 1.0f);
        }
        void drawTooltip (Graphics& g, const String& t, int w, int h) override
        {
            g.fillAll (Colour (0xf0081218));
            g.setColour (white.withAlpha (0.5f)); g.drawRect (0, 0, w, h);
            g.setColour (white); g.setFont (hud (13.0f, true));
            g.drawFittedText (t, Rectangle<int> (w, h).reduced (9, 4), Justification::centredLeft, 3);
        }
    };

    //==========================================================================
    // button as a dark capsule cell; lit rim when on
    struct Cell : public Button
    {
        String a, b;
        int side;
        bool firstIsOn;
        Cell (String first, String second, int s, bool aMeansOn = true)
            : Button ({}), a (std::move (first)), b (std::move (second)), side (s), firstIsOn (aMeansOn)
        {
            setClickingTogglesState (true);
            setMouseCursor (MouseCursor::PointingHandCursor);
        }
        void paintButton (Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (5.0f, 6.0f);
            if (b.isEmpty()) { cellFace (g, r, a, getToggleState() || (down && ! getClickingTogglesState()), over, acc (side)); return; }
            const bool aOn = firstIsOn ? getToggleState() : ! getToggleState();
            cellFace (g, r.withWidth (r.getWidth() * 0.5f - 4.0f), a, aOn, over, acc (side));
            cellFace (g, r.withLeft (r.getCentreX() + 4.0f), b, ! aOn, over, acc (side));
        }
    };

    // a row of capsule faces for a choice parameter (one lit)
    struct Seg : public Component, public SettableTooltipClient
    {
        StringArray opts;
        int side, value = 0, hover = -1;
        float fontH = 9.5f;
        std::unique_ptr<ParameterAttachment> att;
        Seg (RangedAudioParameter& p, StringArray o, int s) : opts (std::move (o)), side (s)
        {
            att = std::make_unique<ParameterAttachment> (p, [this] (float v) { value = (int) std::lround (v); repaint(); });
            att->sendInitialUpdate();
            setMouseCursor (MouseCursor::PointingHandCursor);
        }
        int indexAt (float x) const { return jlimit (0, opts.size() - 1, (int) (x / ((float) getWidth() / (float) opts.size()))); }
        void mouseDown (const MouseEvent& e) override { att->setValueAsCompleteGesture ((float) indexAt (e.position.x)); }
        void mouseMove (const MouseEvent& e) override { const int h = indexAt (e.position.x); if (h != hover) { hover = h; repaint(); } }
        void mouseExit (const MouseEvent&) override { hover = -1; repaint(); }
        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (5.0f, 6.0f);
            const float gap = 6.0f, w = (r.getWidth() - gap * (float) (opts.size() - 1)) / (float) opts.size();
            for (int i = 0; i < opts.size(); ++i)
                cellFace (g, { r.getX() + (w + gap) * (float) i, r.getY(), w, r.getHeight() }, opts[i], i == value, i == hover, acc (side), fontH);
        }
    };

    // HUD panel with the live (modulated) filter response
    struct FilterPlot : public Component
    {
        ChloaelisProcessor& proc;
        int side;
        float lastSig = -1.0f;
        FilterPlot (ChloaelisProcessor& p, int s) : proc (p), side (s) { setInterceptsMouseClicks (false, false); }
        float val (int p) const { return chl::rangeFor (p).convertFrom0to1 (proc.paramNorm[(size_t) side][(size_t) p].load()); }
        void update()
        {
            const float sig = val (chl::cutoff) * 0.001f + val (chl::res) * 7.0f + val (chl::morph) * 13.0f;
            if (std::abs (sig - lastSig) > 1e-4f) { lastSig = sig; repaint(); }
        }
        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (3.0f);
            const float rad = r.getHeight() * 0.5f;
            g.setColour (Colours::black.withAlpha (0.3f)); g.fillRoundedRectangle (r, rad);
            g.setColour (white.withAlpha (0.45f)); g.drawRoundedRectangle (r, rad, 1.0f);
            auto in = r.reduced (rad * 0.6f, 5.0f);
            for (int i = 1; i < 4; ++i)
                dashed (g, { in.getX(), in.getY() + in.getHeight() * (float) i / 4.0f }, { in.getRight(), in.getY() + in.getHeight() * (float) i / 4.0f }, white.withAlpha (0.12f));
            auto fx = [&] (float f) { return in.getX() + in.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); };
            auto dy = [&] (float db) { return in.getY() + in.getHeight() * (1.0f - (db + 36.0f) / 54.0f); };
            const float fc = val (chl::cutoff), k = 2.0f - 1.96f * val (chl::res), m = jlimit (0.0f, 3.0f, val (chl::morph));
            const int seg = jmin (2, (int) m);
            const float t = m - (float) seg;
            Path p;
            for (int i = 0; i <= 120; ++i)
            {
                const float f = 20.0f * std::pow (1000.0f, (float) i / 120.0f);
                const std::complex<float> s (0.0f, f / fc);
                const auto D = s * s + k * s + 1.0f;
                const std::complex<float> resp[4] = { 1.0f / D, k * s / D, s * s / D, (s * s + 1.0f) / D };
                const auto hcx = resp[seg] + (resp[seg + 1] - resp[seg]) * t;
                const float db = jlimit (-36.0f, 18.0f, 20.0f * std::log10 (std::abs (hcx) + 1e-6f));
                const Point<float> q (fx (f), dy (db));
                if (i == 0) p.startNewSubPath (q); else p.lineTo (q);
            }
            glow (g, p, acc (side), 1.4f);
        }
    };

    // tape condition: a strip of oxide that frays and darkens as the tape ages
    struct TapeStrip : public Component
    {
        ChloaelisProcessor& proc;
        int side;
        float last = -1.0f;
        bool lastActive = false;
        TapeStrip (ChloaelisProcessor& p, int s) : proc (p), side (s) { setInterceptsMouseClicks (false, false); }
        bool active() const { return proc.apvts.getRawParameterValue (chl::delayTypeId (side))->load() < 0.5f; }
        void update()
        {
            const float a = proc.tapeAge[(size_t) side].load();
            if (std::abs (a - last) > 0.002f || active() != lastActive) { last = a; lastActive = active(); repaint(); }
        }
        void paint (Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            const float age = jmax (0.0f, last);
            const float al = lastActive ? 1.0f : 0.4f;
            auto strip = b.withHeight (6.0f).withCentre ({ b.getCentreX(), b.getY() + 5.0f }).reduced (4.0f, 0.0f);
            g.setColour (Colours::black.withAlpha (0.4f * al)); g.fillRoundedRectangle (strip.expanded (1.0f), 3.0f);
            Random rng (0x7a9e + side);
            const int n = 30;
            const float sw = strip.getWidth() / (float) n;
            for (int i = 0; i < n; ++i)
            {
                const float scar = rng.nextFloat() < age * 0.6f ? rng.nextFloat() * age : 0.0f;
                g.setColour (acc (side).withMultipliedBrightness (1.0f - 0.55f * age).withAlpha ((0.85f - 0.75f * scar) * al));
                g.fillRect (strip.getX() + sw * (float) i, strip.getY(), sw - 0.6f, strip.getHeight());
            }
            const String s = "TAPE " + String ((int) std::round ((1.0f - age) * 100.0f)) + "%";
            txt (g, s, b.withTrimmedTop (10.0f).withHeight (12.0f), hud (8.5f, true), white.withAlpha (0.7f * al));
        }
    };

    //==========================================================================
    // Text in the cells is blended as "difference" against what's under it: dark over the
    // bright highlights, bright over the dark glass. JUCE has no blend modes, so the cell's
    // cache renders the cell without its labels, renders the labels alone into a mask, and
    // combines the two by hand (only when the cell re-renders, never per frame).
    struct DiffLabel : public Label
    {
        void paint (Graphics& g) override { if (diffTextPass != 1) Label::paint (g); }
    };

    //==========================================================================
    // A cell in the network: a soft dark capsule at its own tilt, a ghost outline tethered
    // beside it, contents kept upright. The first cell of a section carries its callout.
    struct Node : public Component
    {
        float cw, ch, angle, phase, depth;
        bool drawCell, vial = false;     // vial: the cell itself is a knob's glass vial (the knob draws it)
        int side = -1;
        Point<float> home;
        String title, sub;
        Colour rim = white;
        float rimAlpha = 0.22f, blurRadius = 0.0f;
        float z = 0.0f, lift = 0.0f, persp = 1.0f;   // depth toward the viewer (px), cursor proximity, projected scale
        Point<float> hollow;                           // the camera lean (cell frame) as last rendered: moves the glass highlight
        static constexpr float pad = 34.0f;

        Node (Point<float> centre, float w, float h, float ang, bool cellBg, float ph, float dz)
            : cw (w), ch (h), angle (ang), phase (ph), depth (dz), drawCell (cellBg), home (centre)
        {
            const float ca = std::abs (std::cos (ang)), sa = std::abs (std::sin (ang));
            setBounds (Rectangle<float> (w * ca + h * sa + pad * 2, w * sa + h * ca + pad * 2).withCentre (centre).toNearestInt());
            setCachedComponentImage (cache = new BlurCache (*this, blurRadius));
            cache->differenceText = true;
        }
        BlurCache* cache = nullptr;     // owned by the component
        // drift / parallax: move without throwing the cached render away
        void moveTo (const AffineTransform& t)
        {
            cache->holdForMove = true;
            setTransform (t);
            cache->holdForMove = false;
        }
        Point<float> c() const { return getLocalBounds().toFloat().getCentre(); }
        bool hitTest (int x, int y) override
        {
            // inside the (tilted) capsule, with a little margin
            const auto p = Point<float> ((float) x, (float) y) - c();
            const float ca = std::cos (-angle), sa = std::sin (-angle);
            const float lx = p.x * ca - p.y * sa, ly = p.x * sa + p.y * ca;
            return std::abs (lx) <= cw * 0.5f + 6.0f && std::abs (ly) <= ch * 0.5f + 6.0f;
        }
        template <typename T> T* place (T* comp, Point<float> offset, float w, float h)
        {
            addAndMakeVisible (comp);
            comp->setBounds (Rectangle<float> (w, h).withCentre (c() + offset).toNearestInt());
            return comp;
        }
        void paint (Graphics& g) override
        {
            const auto r = Rectangle<float> (cw, ch).withCentre (c());
            if (drawCell && vial)
            {
                const auto gh = r.translated (std::cos (angle) * 13.0f, std::sin (angle) * 13.0f + 6.0f);
                ghostCapsule (g, gh, angle + 0.12f, 0.24f);
                dashed (g, c(), gh.getCentre(), white.withAlpha (0.3f), 0.8f);
            }
            else if (drawCell)
            {
                const auto gh = r.translated (std::cos (angle) * 13.0f, std::sin (angle) * 13.0f + 6.0f);
                ghostCapsule (g, gh, angle + 0.12f, 0.24f);
                dashed (g, c(), gh.getCentre(), white.withAlpha (0.3f), 0.8f);
                glassCapsule (g, r, angle, side < 0 ? acc (-1) : acc (side), jlimit (0.0f, 1.0f, (rimAlpha - 0.22f) * 1.5f), hollow);
            }
        }
    };

    // one collar ring around a capsule of width w, its top face centred at (cx, yy)
    void collarRing (Graphics& g, float cx, float yy, float w, Colour ac, bool hover)
    {
        const float rw = w + 9.0f, rh = 10.0f, fh = 6.0f, rx = cx - rw * 0.5f;
        ColourGradient bg (ac.withMultipliedBrightness (0.45f), rx, yy, ac.withMultipliedBrightness (0.45f), rx + rw, yy, false);
        bg.addColour (0.25, ac.withMultipliedBrightness (1.15f));
        bg.addColour (0.33, ac.interpolatedWith (Colours::white, 0.55f));
        bg.addColour (0.55, ac.withMultipliedBrightness (0.8f));
        g.setGradientFill (bg);
        g.fillRoundedRectangle (rx, yy, rw, rh, 2.0f);
        g.fillEllipse (rx, yy + rh - fh * 0.5f, rw, fh);
        g.setColour (ac.interpolatedWith (Colours::white, 0.45f));
        g.fillEllipse (rx, yy - fh * 0.5f, rw, fh);
        g.setColour (Colour (12, 16, 19));
        g.fillEllipse (cx - w * 0.5f, yy - fh * 0.32f, w, fh * 0.64f);
        g.setColour (Colours::white.withAlpha (hover ? 0.95f : 0.75f));
        g.drawEllipse (rx, yy - fh * 0.5f, rw, fh, 0.8f);
        g.setColour (ac.withAlpha (hover ? 0.35f : 0.2f));
        g.drawRoundedRectangle (rx - 2.0f, yy - fh * 0.5f - 2.0f, rw + 4.0f, rh + fh + 4.0f, 5.0f, 2.0f);
    }

    // The vessel (standalone only): drop an audio file into the glass. Its waveform runs up
    // the inside (start of the file at the bottom), two rings mark the loop, a bead rides the
    // playhead, and the liquid level is its volume. Drag a ring to move that loop point, drag
    // anywhere else to set the level, double-click to choose a file.
    struct VesselView : public Component, public FileDragAndDropTarget
    {
        ChloaelisProcessor& proc;
        Rectangle<float> body;
        const Point<float>* view = nullptr;
        bool dragOver = false;
        int dragging = 0;            // 1 = start ring, 2 = end ring, 3 = level
        float levelAtDown = 0.0f;
        float lastPos = -1.0f;
        VialMotion mo;
        std::unique_ptr<FileChooser> chooser;

        explicit VesselView (ChloaelisProcessor& p) : proc (p) { setMouseCursor (MouseCursor::PointingHandCursor); }

        float yFor (float t) const { return body.getBottom() - 8.0f - jlimit (0.0f, 1.0f, t) * (body.getHeight() - 16.0f); }
        float tFor (float y) const { return jlimit (0.0f, 1.0f, (body.getBottom() - 8.0f - y) / (body.getHeight() - 16.0f)); }

        void poll (int frame)
        {
            mo.time += 1.0f / 60.0f;
            mo.phase += 0.06f;
            mo.amp = 0.6f;
            const float p = proc.vesselPos.load();
            const bool moving = proc.vesselForUi() != nullptr && std::abs (p - lastPos) > 0.0005f;
            if ((moving && frame % 2 == 0) || frame % 6 == 0) { lastPos = p; repaint(); }
        }

        void paint (Graphics& g) override
        {
            const auto ac = acc (-1);
            const auto clip = proc.vesselForUi();
            const bool hover = isMouseOverOrDragging() || dragOver;
            glassVial (g, body, 0.0f, ac, proc.vesselLevel.load(), -1.0f, dragOver ? 1.0f : 0.0f, hover, view != nullptr ? *view : Point<float>(), mo);
            Graphics::ScopedSaveState ss (g);
            Path cap; cap.addRoundedRectangle (body, body.getWidth() * 0.5f);
            g.reduceClipRegion (cap);
            if (clip == nullptr)
            {
                // empty: an arrow into the glass
                txt (g, String::fromUTF8 ("\xe2\x86\x93"), body.withSizeKeepingCentre (40, 40), greek (26.0f), white.withAlpha (dragOver ? 1.0f : 0.6f));
                return;
            }
            const float s = proc.vesselStart.load(), e = proc.vesselEnd.load();
            const float lo = jmin (s, e), hi = jmax (s, e);
            // the waveform, bottom (start) to top (end); brighter inside the loop
            const int n = (int) clip->peaks.size();
            const float halfW = body.getWidth() * 0.42f;
            Path inLoop, outLoop;
            for (int k = 0; k < n; ++k)
            {
                const float t = ((float) k + 0.5f) / (float) n;
                const float y = yFor (t), w = jmax (0.5f, clip->peaks[(size_t) k] * halfW * 1.6f);
                (t >= lo && t <= hi ? inLoop : outLoop).addRectangle (body.getCentreX() - jmin (w, halfW), y - 0.35f, 2.0f * jmin (w, halfW), 0.7f);
            }
            g.setColour (white.withAlpha (0.18f)); g.fillPath (outLoop);
            g.setColour (white.withAlpha (0.62f)); g.fillPath (inLoop);
            // the playhead
            const float py = yFor (proc.vesselPos.load());
            g.setColour (ac.withAlpha (0.35f));
            g.fillRect (body.getX(), py - 1.5f, body.getWidth(), 3.0f);
            g.setColour (white);
            g.fillRect (body.getX() + 4.0f, py - 0.5f, body.getWidth() - 8.0f, 1.0f);
        }
        void paintOverChildren (Graphics& g) override
        {
            if (proc.vesselForUi() == nullptr) return;
            const auto ac = acc (-1);
            const float s = proc.vesselStart.load(), e = proc.vesselEnd.load();
            collarRing (g, body.getCentreX(), yFor (s) - 5.0f, body.getWidth(), ac, dragging == 1);
            collarRing (g, body.getCentreX(), yFor (e) - 5.0f, body.getWidth(), ac.interpolatedWith (white, 0.3f), dragging == 2);
        }

        bool hitTest (int x, int y) override { return body.expanded (8.0f).contains ((float) x, (float) y); }
        void mouseDown (const MouseEvent& ev) override
        {
            const float y = ev.position.y;
            const float ys = yFor (proc.vesselStart.load()), ye = yFor (proc.vesselEnd.load());
            const bool has = proc.vesselForUi() != nullptr;
            if (has && std::abs (y - ys) < 10.0f && std::abs (y - ys) <= std::abs (y - ye)) dragging = 1;
            else if (has && std::abs (y - ye) < 10.0f) dragging = 2;
            else { dragging = 3; levelAtDown = proc.vesselLevel.load(); }
            repaint();
        }
        void mouseDrag (const MouseEvent& ev) override
        {
            if (dragging == 1) proc.vesselStart.store (jmin (tFor (ev.position.y), proc.vesselEnd.load() - 0.005f));
            else if (dragging == 2) proc.vesselEnd.store (jmax (tFor (ev.position.y), proc.vesselStart.load() + 0.005f));
            else if (dragging == 3) proc.vesselLevel.store (jlimit (0.0f, 1.0f, levelAtDown - (float) ev.getDistanceFromDragStartY() / 180.0f));
            repaint();
        }
        void mouseUp (const MouseEvent&) override { dragging = 0; repaint(); }
        void mouseDoubleClick (const MouseEvent&) override
        {
            chooser = std::make_unique<FileChooser> ("Pour audio into the vessel", File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.caf");
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
            {
                if (fc.getResult().existsAsFile()) { proc.loadVesselFile (fc.getResult()); repaint(); }
            });
        }
        static bool isAudio (const String& path)
        {
            for (auto* ext : { ".wav", ".aif", ".aiff", ".flac", ".mp3", ".m4a", ".ogg", ".caf" })
                if (path.endsWithIgnoreCase (ext)) return true;
            return false;
        }
        bool isInterestedInFileDrag (const StringArray& files) override { for (auto& f : files) if (isAudio (f)) return true; return false; }
        void fileDragEnter (const StringArray&, int, int) override { dragOver = true; repaint(); }
        void fileDragExit (const StringArray&) override { dragOver = false; repaint(); }
        void filesDropped (const StringArray& files, int, int) override
        {
            dragOver = false;
            for (auto& f : files)
                if (isAudio (f) && proc.loadVesselFile (File (f))) break;
            repaint();
        }
    };

    // The help screen: the key to every symbol and how to play it. Opened with the ? cell,
    // closed by clicking anywhere (or Esc in the desktop app).
    struct HelpOverlay : public Component
    {
        bool desktop = false;
        HelpOverlay() { setBufferedToImage (true); }
        void mouseDown (const MouseEvent&) override { setVisible (false); }

        void paint (Graphics& g) override
        {
            g.fillAll (Colour (0xf2080c10));
            const auto area = getLocalBounds().toFloat().reduced (70.0f, 46.0f);
            txt (g, String::fromUTF8 ("CHLOAELIS  \xc2\xb7  HELP"), area.withHeight (30), greek (24.0f).withExtraKerningFactor (0.3f), white);
            txt (g, "CLICK ANYWHERE TO CLOSE", area.withHeight (30).translated (0, 28), hud (9.5f), white.withAlpha (0.55f));

            struct Row { const char* sym; const char* text; };
            const Row strip[] = {
                { "\xce\xb3", "input level" },
                { "\xcf\x89", "filter cutoff: drag the ring up and down (a ghost ring shows modulation)" },
                { "\xce\xb6", "resonance" },
                { "\xce\xbc", "filter mode: low pass > band pass > high pass > notch" },
                { "\xce\xba", "tube drive" },
                { "I-IV", "tube type: I 12AX7 (default), II EL84, III 6L6 with sag, IV starved" },
                { "\xcf\x84", "delay time" },
                { "\xcf\x86", "regen: the delay's feedback (past ~90% it sings on its own)" },
                { "\xce\xb7", "delay mix, dry to wet" },
                { "\xce\xbe", "random freeze: the chance the buffer gets held at random" },
                { "\xce\xa9", "output level" },
                { "\xe2\x88\xbf", "LFO: the dial is its rate, the vial its depth (the letter says what it moves)" } };
            const Row shared[] = {
                { "\xce\xbd", "Lorenz speed" },
                { "\xcf\x81", "Lorenz chaos" },
                { "\xce\xb2", "Lorenz shape" },
                { "\xce\x94", "how far the Lorenz attractor moves both cutoffs" },
                { "\xcf\x88", "poly: up to 8 delay taps per side, a reverb-like cloud at the top" },
                { "\xcf\x87", "flock: a boids swarm moves the poly taps' times" },
                { "\xcf\x83", "level of the built-in recording (fade it in and out)" },
                { "\xe2\x88\x9e \xe2\x89\x88", "which recording: the loop sign is the base recording, the wave sign the hydrophone" },
                { "\xce\xa6", "master feedback, crossed: left out > right in, right out > left in" },
                { "\xe2\x86\xba", "reset everything to the defaults" } };

            auto column = [&] (Rectangle<float> r, const char* head, const Row* rows, int n)
            {
                txt (g, head, r.removeFromTop (24), hud (11.0f), white.withAlpha (0.8f), Justification::centredLeft);
                for (int i = 0; i < n; ++i)
                {
                    auto line = r.removeFromTop (31);
                    txt (g, String::fromUTF8 (rows[i].sym), line.removeFromLeft (56), greek (19.0f), white, Justification::centredLeft);
                    g.setFont (hud (11.5f)); g.setColour (white.withAlpha (0.85f));
                    g.drawFittedText (String::fromUTF8 (rows[i].text).toUpperCase(), line.toNearestInt(), Justification::centredLeft, 2, 0.8f);
                }
            };
            auto body = area.withTrimmedTop (70);
            auto left = body.removeFromLeft (body.getWidth() * 0.5f).reduced (10, 0);
            auto right = body.reduced (10, 0);
            column (left, "EACH SIDE  (LEFT IN > LEFT OUT, RIGHT IN > RIGHT OUT)", strip, (int) std::size (strip));
            column (right.removeFromTop (24 + 31 * (int) std::size (shared)), "SHARED (MIDDLE)", shared, (int) std::size (shared));

            right.removeFromTop (14);
            txt (g, "HOW TO PLAY", right.removeFromTop (24), hud (11.0f), white.withAlpha (0.8f), Justification::centredLeft);
            String how = String::fromUTF8 ("drag any dial or glass up and down; double-click a dial to reset it.  "
                                           "hover a cell to bring it into focus.  "
                                           "switches (tube on, tape / pt2399 / bbd, slew / fade, filter \xe2\x80\xba dly) crossfade.  "
                                           "freeze holds the delay, refresh puts on fresh tape, hpf thins the feedback.  "
                                           "it remembers how you left it.");
            if (desktop)
                how += String::fromUTF8 ("  vessel: drop an audio file in (or double-click it), drag the rings to set the loop, "
                                         "drag the glass for its level.  \xe2\xa4\xa2 or cmd+F: fullscreen, esc to leave.");
            g.setFont (hud (11.5f)); g.setColour (white.withAlpha (0.85f));
            g.drawFittedText (how.toUpperCase(), right.toNearestInt(), Justification::topLeft, 12, 0.9f);
        }
    };

    // section callouts, drawn above all the cells. Each is rendered once into an image (text
    // layout is the expensive part) and then just positioned and faded every frame.
    struct Callouts : public Component
    {
        struct Cached { String key; Image img; float scale = 0; };
        std::vector<Node*> nodes;
        std::vector<Cached> cache;
        std::function<float (Point<float>)> blurAt;
        static constexpr float iw = 520.0f, ih = 60.0f;      // image extent, anchored at p0 (the dot)
        Callouts() { setInterceptsMouseClicks (false, false); }

        static void render (Graphics& g, const Node& n, float sx)
        {
            // drawn relative to p0 placed at (iw/2, ih - 6)
            const Point<float> p0 (iw * 0.5f, ih - 6.0f);
            const auto p1 = p0 + Point<float> (sx * 14.0f, -16.0f);
            const auto p2 = p1 + Point<float> (sx * 54.0f, 0.0f);
            g.setColour (white.withAlpha (0.7f));
            g.drawLine (Line<float> (p0, p1), 0.8f);
            g.drawLine (Line<float> (p1, p2), 0.8f);
            g.fillEllipse (p0.x - 1.6f, p0.y - 1.6f, 3.2f, 3.2f);
            const Rectangle<float> tr = sx > 0 ? Rectangle<float> (p1.x + 2.0f, p1.y - 15.0f, 230.0f, 13.0f) : Rectangle<float> (p1.x - 232.0f, p1.y - 15.0f, 230.0f, 13.0f);
            const auto j = sx > 0 ? Justification::centredLeft : Justification::centredRight;
            g.setFont (hud (10.0f, true));
            g.setColour (Colours::black.withAlpha (0.5f));
            g.drawText (n.title.toUpperCase(), tr.translated (0.0f, 1.0f), j, false);
            txt (g, n.title.toUpperCase(), tr, hud (10.0f, true), white, j);
            txt (g, n.sub.toUpperCase(), tr.translated (0, 17.0f).withHeight (11.0f), hud (8.0f), white.withAlpha (0.7f), j);
        }

        void paint (Graphics& g) override
        {
            const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
            cache.resize (nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                auto* n = nodes[i];
                if (n->title.isEmpty()) continue;
                const float sx = n->home.x > W * 0.55f ? -1.0f : 1.0f;
                auto& cc = cache[i];
                const String key = n->title + "|" + n->sub;
                if (cc.key != key || std::abs (cc.scale - scale) > 0.001f || cc.img.isNull())
                {
                    cc.key = key; cc.scale = scale;
                    cc.img = Image (Image::ARGB, roundToInt (iw * scale), roundToInt (ih * scale), true, SoftwareImageType());
                    Graphics ig (cc.img);
                    ig.addTransform (AffineTransform::scale (scale));
                    render (ig, *n, sx);
                }
                const auto c = n->getBounds().toFloat().getCentre().transformedBy (n->getTransform());
                const float a = (1.0f - 0.65f * (blurAt ? jlimit (0.0f, 1.0f, blurAt (c) / 6.0f) : 0.0f)) * n->getAlpha();
                const float rr = jmax (n->cw, n->ch) * 0.5f * n->persp;
                const auto p0 = c + Point<float> (sx * rr * 0.55f, -n->ch * n->persp * 0.5f * 0.85f - 4.0f);
                g.setOpacity (a);
                g.drawImage (cc.img, Rectangle<float> (p0.x - iw * 0.5f, p0.y - (ih - 6.0f), iw, ih), RectanglePlacement::stretchToFit, false);
            }
        }
    };

    // the dashed links between cells (they follow the cells as they drift) and, under every
    // cell, a glow in the colour of that side's output level
    struct Links : public Component
    {
        struct L { Component* a; Component* b; int side; };
        std::vector<L> links;
        std::vector<Node*> nodes;
        float db[2] { -90.0f, -90.0f };
        std::function<float (Point<float>)> blurAt;
        TintedGlow glows[3];
        Links() { setInterceptsMouseClicks (false, false); }
        static Point<float> centreOf (Component* c) { return c->getBounds().toFloat().getCentre().transformedBy (c->getTransform()); }
        float activity (int side) const
        {
            const float d = side < 0 ? jmax (db[0], db[1]) : db[side];
            return jlimit (0.0f, 1.0f, (d + 54.0f) / 48.0f);
        }
        Colour levelColour (int side) const
        {
            if (side < 0) return heat (db[0] > db[1] ? 0 : 1, jmax (db[0], db[1]));
            return heat (side, db[side]);
        }
        void paint (Graphics& g) override
        {
            for (auto* n : nodes)
            {
                const float act = activity (n->side) * n->getAlpha();
                if (act < 0.02f) continue;
                const auto c = centreOf (n);
                const float r = (jmax (n->cw, n->ch) * 0.62f + 14.0f) * n->persp;
                drawGlow (g, glows[(size_t) (n->side + 1)], c, r, levelColour (n->side).withAlpha (0.30f * act));
            }
            for (auto& l : links)
            {
                const auto a = centreOf (l.a), b = centreOf (l.b);
                const float soft = blurAt ? jlimit (0.0f, 1.0f, blurAt ((a + b) * 0.5f) / 6.0f) : 0.0f;
                const auto d = b - a;
                const float len = d.getDistanceFromOrigin();
                if (len < 1.0f) continue;
                const auto u = d / len;
                // start and end at the cell edges
                const auto s0 = a + u * 34.0f, s1 = b - u * 34.0f;
                if ((s1 - s0).getDotProduct (u) <= 0) continue;
                const float act = activity (l.side);
                const auto col = white.interpolatedWith (levelColour (l.side), 0.25f + 0.6f * act);
                Path dp;
                const float w = 1.0f - 0.3f * soft, segLen = (s1 - s0).getDistanceFromOrigin();
                for (float t = 0.0f; t < segLen; t += 8.0f)
                    dp.addLineSegment ({ s0 + u * t, s0 + u * jmin (segLen, t + 4.0f) }, w);
                g.setColour (col.withAlpha ((0.36f + 0.3f * act) * (1.0f - 0.6f * soft)));
                g.fillPath (dp);
                Path dots;
                dots.addEllipse (s0.x - 1.8f, s0.y - 1.8f, 3.6f, 3.6f);
                dots.addEllipse (s1.x - 1.8f, s1.y - 1.8f, 3.6f, 3.6f);
                g.setColour (col.withAlpha (0.55f * (1.0f - 0.6f * soft)));
                g.fillPath (dots);
            }
        }
    };

    //==========================================================================
    // the field: deep gradient, faded holographic text, static dust
    struct Backdrop : public Component
    {
        Backdrop() { setBufferedToImage (true); setInterceptsMouseClicks (false, false); }
        void paint (Graphics& g) override
        {
            // the field is centred on the design area, and reaches past it on a wider / taller window
            const float fw = (float) getWidth(), fh = (float) getHeight(), ox = (fw - W) * 0.5f, oy = (fh - H) * 0.5f;
            g.setGradientFill (ColourGradient (pal.field1, ox + W * 0.48f, oy + H * 0.38f, pal.field0, ox + W * 0.95f + (fw - W) * 0.3f, oy + H * 1.05f + (fh - H) * 0.3f, true));
            g.fillAll();
            g.setGradientFill (ColourGradient (pal.haze, ox + W * 0.2f, oy + H * 0.8f, pal.haze.withAlpha (0.0f), ox + W * 0.55f, oy + H * 0.8f, true));
            g.fillRect (getLocalBounds());
            Random rng (0x5711ce);
            for (int i = 0; i < (int) (320.0f * fw * fh / (W * H)); ++i)
            {
                const float x = rng.nextFloat() * fw, y = rng.nextFloat() * fh, r = 0.6f + rng.nextFloat() * 1.4f;
                g.setColour (pal.mote.withAlpha (0.08f + 0.22f * rng.nextFloat()));
                g.fillEllipse (x - r, y - r, r * 2, r * 2);
            }
        }
    };

    // drifting bokeh discs and motes, shifting against the mouse (parallax)
    struct Bokeh : public Component
    {
        struct D { float x, y, r, a, sp, ph, depth; };
        std::vector<D> discs;
        Point<float> parallax;
        TintedGlow glow;
        Bokeh()
        {
            setInterceptsMouseClicks (false, false);
            Random rng (0xb0ce);
            for (int i = 0; i < 40; ++i)
                discs.push_back ({ rng.nextFloat() * W, rng.nextFloat() * H, 4.0f + rng.nextFloat() * (i < 9 ? 60.0f : 14.0f), 0.03f + rng.nextFloat() * 0.07f,
                                   3.0f + rng.nextFloat() * 8.0f, rng.nextFloat() * 6.28f, 0.2f + rng.nextFloat() * 1.0f });
        }
        void paint (Graphics& g) override
        {
            const double t = now();
            for (auto& d : discs)
            {
                const float fw = (float) getWidth(), fh = (float) getHeight();
                const float x = std::fmod (d.x * fw / W + (float) t * d.sp + 2000.0f, fw + 120.0f) - 60.0f + parallax.x * d.depth;
                const float y = d.y * fh / H + 10.0f * (float) std::sin (t * 0.2 + d.ph) + parallax.y * d.depth;
                drawGlow (g, glow, { x, y }, d.r, pal.mote.withAlpha (d.a));
                if (d.r > 20.0f)
                {
                    g.setOpacity (1.0f);
                    g.setColour (pal.mote.withAlpha (d.a * 0.6f));
                    g.drawEllipse (x - d.r * 0.85f, y - d.r * 0.85f, d.r * 1.7f, d.r * 1.7f, 1.0f);
                }
            }
        }
    };

    // The Lorenz attractor as a hologram floating in the field behind the middle cells. A faint
    // "ghost" orbit (integrated here at a steady pace, so the shape always shows) is drawn as
    // dust whose near points swell into soft bokeh; the real trail (the processor's attractor,
    // followed closely) is a brighter thread with a glowing head, and that head sends dashed
    // lines to both cutoffs: brighter with the Depth knob and with how far it is pushing.
    // The dust is splatted on the CPU at the screen's pixel density into one image per frame.
    struct Hologram : public Component
    {
        ChloaelisProcessor& proc;
        struct P3 { float x, y, z; };
        std::vector<P3> ghost, trail;
        size_t gHead = 0, tHead = 0, gCount = 0, tCount = 0;
        double gx = 1.0, gy = 1.0, gz = 20.0, qx = 0.1, qy = 0.0, qz = 20.0;
        Image img;
        float hs = 1.0f;                  // dust image px per design px (follows the screen's pixel scale)
        std::vector<Path> trailPaths;     // drawn as vectors on the GPU, so the thread stays crisp
        TintedGlow headGlow, sendGlow[2];
        Component* cut[2] {};
        float spin = 0.0f, camYaw = 0.0f, camPitch = 0.0f, depth = 0.0f, send[2] {}, march = 0.0f;
        double rho = 28.0, beta = 8.0 / 3.0;
        Point<float> parallax, headPos;
        static constexpr float rx = 290.0f, ry = 170.0f, rw = 620.0f, rh = 500.0f, sc = 84.0f;

        explicit Hologram (ChloaelisProcessor& p) : proc (p)
        {
            setInterceptsMouseClicks (false, false);
            ghost.resize (2600); trail.resize (700);
            for (int i = 0; i < 3000; ++i) stepGhost (0.004);        // settle onto the attractor
        }
        static void lorenzStep (double& x, double& y, double& z, double h, double r, double b)
        {
            // midpoint (RK2) is plenty for a picture
            const double dx = 10.0 * (y - x), dy = x * (r - z) - y, dz = x * y - b * z;
            const double mx = x + dx * h * 0.5, my = y + dy * h * 0.5, mz = z + dz * h * 0.5;
            x += h * 10.0 * (my - mx); y += h * (mx * (r - mz) - my); z += h * (mx * my - b * mz);
            if (! std::isfinite (x) || std::abs (x) > 1e4) { x = 0.1; y = 0.0; z = 20.0; }
        }
        void stepGhost (double h)
        {
            lorenzStep (gx, gy, gz, h, rho, beta);
            ghost[gHead] = { (float) gx, (float) gy, (float) gz };
            gHead = (gHead + 1) % ghost.size(); gCount = jmin (gCount + 1, ghost.size());
        }
        void advance (float dt)
        {
            const double chaos = proc.apvts.getRawParameterValue (chl::lzChaosId)->load();
            rho  = 24.5 + chaos * chaos * 155.5;
            beta = proc.apvts.getRawParameterValue (chl::lzShapeId)->load();
            const double speed = 0.03 * std::exp2 (proc.apvts.getRawParameterValue (chl::lzSpeedId)->load() * 9.0);
            depth = proc.apvts.getRawParameterValue (chl::lzDepthId)->load();

            for (int i = 0; i < 8; ++i) stepGhost (0.0035);          // the shape: steady, whatever the speed

            // the real path: integrate along, then lean onto the processor's actual state
            const double simT = speed * dt;
            const int n = jlimit (1, 40, (int) std::ceil (simT / 0.004));
            for (int i = 0; i < n; ++i)
            {
                lorenzStep (qx, qy, qz, simT / n, rho, beta);
                trail[tHead] = { (float) qx, (float) qy, (float) qz };
                tHead = (tHead + 1) % trail.size(); tCount = jmin (tCount + 1, trail.size());
            }
            qx += (proc.lzX.load() - qx) * 0.3; qy += (proc.lzY.load() - qy) * 0.3; qz += (proc.lzZ.load() - qz) * 0.3;
            for (int s = 0; s < 2; ++s) send[s] += (proc.lzOut[(size_t) s].load() - send[s]) * 0.3f;

            spin += dt * 0.07f;
            march += dt * (10.0f + 40.0f * depth);
            render();
        }
        // attractor coordinates -> design px (x, y) and nearness (-1 far .. 1 near)
        Point<float> project (const P3& p, float& near) const
        {
            const float s = (float) std::sqrt (jmax (1.0, beta * (rho - 1.0)));
            const float X = p.x / s, Y = p.y / s, Z = (p.z - (float) (rho - 1.0)) / s;
            const float a = spin + camYaw * 0.45f, ca = std::cos (a), sa = std::sin (a);
            const float x1 = X * ca - Y * sa, y1 = X * sa + Y * ca;
            const float t = 0.28f + camPitch * 0.18f, ct = std::cos (t), st = std::sin (t);
            const float z2 = Z * ct - y1 * st, d = Z * st + y1 * ct;
            near = jlimit (-1.0f, 1.0f, d / 2.2f);
            const float f = 1.0f / (1.0f - d * 0.12f);
            return { W * 0.5f + x1 * sc * f, 405.0f - z2 * sc * f };
        }
        void render()
        {
            const int iw = roundToInt (rw * hs), ih = roundToInt (rh * hs);
            if (img.getWidth() != iw || img.getHeight() != ih)
            {
                img = Image (Image::ARGB, iw, ih, true, SoftwareImageType());
                img.setBackupEnabled (false);
            }
            img.clear (img.getBounds());
            const auto col = pal.mote;
            Image::BitmapData bd (img, Image::BitmapData::readWrite);
            // a soft disc (alpha falls off linearly to the rim), blended straight into the pixels:
            // far cheaper than thousands of resampled sprite draws, so it can run at full resolution
            auto splat = [&] (Point<float> p, float r, float op)
            {
                float rp = r * hs;
                if (rp < 1.5f) { op *= (rp * rp) / 2.25f; rp = 1.5f; }   // keep tiny dots' weight, avoid sparkle
                const float cx = (p.x - rx) * hs, cy = (p.y - ry) * hs, inv = 1.0f / rp;
                const int x0 = jmax (0, (int) (cx - rp)), x1 = jmin (bd.width - 1, (int) (cx + rp) + 1);
                const int y0 = jmax (0, (int) (cy - rp)), y1 = jmin (bd.height - 1, (int) (cy + rp) + 1);
                const float o255 = op * 255.0f;
                const PixelARGB base (col.withAlpha (1.0f).getPixelARGB());
                for (int y = y0; y <= y1; ++y)
                {
                    const float dy = ((float) y + 0.5f - cy) * inv;
                    auto* px = reinterpret_cast<PixelARGB*> (bd.getLinePointer (y)) + x0;
                    for (int x = x0; x <= x1; ++x, ++px)
                    {
                        const float dx = ((float) x + 0.5f - cx) * inv, d2 = dx * dx + dy * dy;
                        if (d2 >= 1.0f) continue;
                        const int a = (int) (o255 * (1.0f - std::sqrt (d2)));
                        if (a <= 0) continue;
                        PixelARGB src (base);
                        src.multiplyAlpha (jmin (a, 255));
                        px->blend (src);
                    }
                }
            };

            // ghost orbit as dust; near points open up into soft discs
            for (size_t k = 0; k < gCount; ++k)
            {
                const size_t i = (gHead + ghost.size() - gCount + k) % ghost.size();
                float nr;
                const auto p = project (ghost[i], nr);
                const float nn = (nr + 1.0f) * 0.5f;
                const float r = 1.3f + nn * nn * nn * 10.0f;
                const float age = 0.35f + 0.65f * (float) k / (float) gCount;
                splat (p, r, jlimit (0.0f, 1.0f, age * (0.30f + 0.45f * (1.0f - nn)) / (0.7f + r * 0.16f)));
            }
            // the real trail: a thread that fades toward its tail (stroked here, filled in paint)
            constexpr int chunks = 14;
            trailPaths.resize (chunks);
            const size_t per = jmax ((size_t) 2, tCount / chunks);
            for (int c = 0; c < chunks; ++c)
            {
                Path path;
                bool started = false;
                for (size_t k = (size_t) c * per; k <= jmin (tCount - 1, (size_t) (c + 1) * per); ++k)
                {
                    float nr;
                    const auto p = project (trail[(tHead + trail.size() - tCount + k) % trail.size()], nr);
                    if (! started) { path.startNewSubPath (p); started = true; } else path.lineTo (p);
                }
                const float f = (float) (c + 1) / chunks;
                trailPaths[(size_t) c].clear();
                PathStrokeType (1.4f + 1.2f * f, PathStrokeType::curved, PathStrokeType::rounded).createStrokedPath (trailPaths[(size_t) c], path);
            }
            if (tCount > 0)
            {
                float nr;
                headPos = project (trail[(tHead + trail.size() - 1) % trail.size()], nr);
            }
        }
        void paint (Graphics& g) override
        {
            // render the dust at the screen's pixel density next frame (capped, it's CPU work)
            hs = jlimit (0.5f, 2.0f, std::ceil (g.getInternalContext().getPhysicalPixelScaleFactor() * 4.0f) / 4.0f);
            g.setOpacity (1.0f);
            if (img.isValid())
                g.drawImageTransformed (img, AffineTransform::scale ((rw / (float) img.getWidth()), rh / (float) img.getHeight()).translated (rx + parallax.x, ry + parallax.y), false);
            {
                Graphics::ScopedSaveState ss (g);
                g.addTransform (AffineTransform::translation (parallax));
                for (size_t c = 0; c < trailPaths.size(); ++c)
                {
                    const float f = (float) (c + 1) / (float) trailPaths.size();
                    g.setColour (pal.mote.withAlpha (0.55f * f * f));
                    g.fillPath (trailPaths[c]);
                }
            }
            const auto hp = headPos + parallax;

            // the sends: head -> each cutoff, marching toward it
            for (int s = 0; s < 2; ++s)
            {
                if (cut[s] == nullptr) continue;
                const auto b = Links::centreOf (cut[s]);
                const auto d = b - hp;
                const float len = d.getDistanceFromOrigin();
                if (len < 60.0f) continue;
                const auto u = d / len;
                const auto s0 = hp + u * 12.0f, s1 = b - u * 38.0f;
                const float segLen = (s1 - s0).getDistanceFromOrigin();
                const float amt = std::abs (send[s]);
                const float a = (0.2f + 0.6f * depth) * (0.45f + 0.55f * amt);
                Path dp;
                for (float t = std::fmod (march, 9.0f); t < segLen; t += 9.0f)
                    dp.addLineSegment ({ s0 + u * t, s0 + u * jmin (segLen, t + 4.0f) }, 1.1f);
                const auto c = acc (s).interpolatedWith (white, 0.35f);
                g.setColour (c.withAlpha (a));
                g.fillPath (dp);
                // a bead riding the send: where it sits says how hard it's pushing (and which way)
                const auto bead = s0 + u * (segLen * (0.5f + 0.45f * send[s]));
                drawGlow (g, sendGlow[s], bead, 8.0f + 6.0f * amt * depth, c.withAlpha (0.25f + 0.6f * depth));
            }
            drawGlow (g, headGlow, hp, 30.0f, pal.mote.withAlpha (0.55f));
            g.setOpacity (1.0f);
            g.setColour (white.withAlpha (0.9f));
            g.fillEllipse (hp.x - 2.4f, hp.y - 2.4f, 4.8f, 4.8f);
        }
    };

    // The poly flock as a hologram in the field behind the poly cells, drawn like the Lorenz
    // one: each boid leaves a dust trail (splatted on the CPU at the screen's pixel density,
    // into one image per frame), its head glows in its side's colour, neighbours flying close
    // are joined by a faint web, and a dashed send marches from the flock to the poly knob.
    // It all brightens with the flock (chi) amount.
    struct FlockView : public Component
    {
        ChloaelisProcessor& proc;
        static constexpr int n = chl::dsp::Flock::n, hist = 130;
        Point<float> trail[n][hist];
        int head = 0, count = 0;
        Image img;
        float hs = 1.0f, amt = 0.0f, march = 0.0f;
        TintedGlow glow[2], sendGlow;
        Component* poly = nullptr;
        Point<float> parallax;
        static constexpr float rx = 300.0f, ry = 545.0f, rw = 600.0f, rh = 270.0f;

        explicit FlockView (ChloaelisProcessor& p) : proc (p) { setInterceptsMouseClicks (false, false); }
        static Point<float> toScreen (float x, float y) { return { rx + rw * 0.5f + x * rw * 0.45f, ry + rh * 0.5f + y * rh * 0.42f }; }
        Point<float> headOf (int i) const { return trail[i][(head + hist - 1) % hist]; }

        void advance (float dt)
        {
            amt += (proc.apvts.getRawParameterValue (chl::flockId)->load() - amt) * 0.1f;
            for (int i = 0; i < n; ++i)
                trail[i][head] = toScreen (proc.flockX[(size_t) i].load (std::memory_order_relaxed), proc.flockY[(size_t) i].load (std::memory_order_relaxed));
            head = (head + 1) % hist;
            count = jmin (count + 1, hist);
            march += dt * (8.0f + 30.0f * amt);
            render();
        }
        void render()
        {
            const int iw = roundToInt (rw * hs), ih = roundToInt (rh * hs);
            if (img.getWidth() != iw || img.getHeight() != ih)
            {
                img = Image (Image::ARGB, iw, ih, true, SoftwareImageType());
                img.setBackupEnabled (false);
            }
            img.clear (img.getBounds());
            Image::BitmapData bd (img, Image::BitmapData::readWrite);
            const float bright = 0.45f + 0.55f * amt;
            for (int i = 0; i < n; ++i)
            {
                const PixelARGB base (acc (i < n / 2 ? 0 : 1).interpolatedWith (pal.mote, 0.35f).withAlpha (1.0f).getPixelARGB());
                for (int k = 0; k < count; ++k)
                {
                    const float age = (float) (k + 1) / (float) count;          // 0 old .. 1 new
                    const auto p = trail[i][(head + hist - count + k) % hist];
                    float rp = (0.9f + 1.4f * age) * hs, op = (0.06f + 0.5f * age * age) * bright;
                    if (rp < 1.5f) { op *= (rp * rp) / 2.25f; rp = 1.5f; }
                    const float cx = (p.x - rx) * hs, cy = (p.y - ry) * hs, inv = 1.0f / rp;
                    const int x0 = jmax (0, (int) (cx - rp)), x1 = jmin (bd.width - 1, (int) (cx + rp) + 1);
                    const int y0 = jmax (0, (int) (cy - rp)), y1 = jmin (bd.height - 1, (int) (cy + rp) + 1);
                    const float o255 = op * 255.0f;
                    for (int y = y0; y <= y1; ++y)
                    {
                        const float dy = ((float) y + 0.5f - cy) * inv;
                        auto* px = reinterpret_cast<PixelARGB*> (bd.getLinePointer (y)) + x0;
                        for (int x = x0; x <= x1; ++x, ++px)
                        {
                            const float dx = ((float) x + 0.5f - cx) * inv, d2 = dx * dx + dy * dy;
                            if (d2 >= 1.0f) continue;
                            const int a = (int) (o255 * (1.0f - std::sqrt (d2)));
                            if (a <= 0) continue;
                            PixelARGB src (base);
                            src.multiplyAlpha (jmin (a, 255));
                            px->blend (src);
                        }
                    }
                }
            }
        }
        void paint (Graphics& g) override
        {
            hs = jlimit (0.5f, 2.0f, std::ceil (g.getInternalContext().getPhysicalPixelScaleFactor() * 4.0f) / 4.0f);
            if (count == 0) return;
            Graphics::ScopedSaveState ss (g);
            g.addTransform (AffineTransform::translation (parallax));
            g.setOpacity (1.0f);
            if (img.isValid())
                g.drawImageTransformed (img, AffineTransform::scale (rw / (float) img.getWidth(), rh / (float) img.getHeight()).translated (rx, ry), false);
            const float bright = 0.3f + 0.7f * amt;

            // the web: boids flying close together
            Path web;
            Point<float> centroid;
            for (int i = 0; i < n; ++i)
            {
                const auto a = headOf (i);
                centroid += a / (float) n;
                for (int j = i + 1; j < n; ++j)
                {
                    const auto b = headOf (j);
                    if (a.getDistanceFrom (b) < 80.0f) web.addLineSegment ({ a, b }, 0.6f);
                }
            }
            g.setColour (pal.mote.withAlpha (0.16f * bright));
            g.fillPath (web);

            // heads
            for (int i = 0; i < n; ++i)
            {
                const auto p = headOf (i);
                const auto c = acc (i < n / 2 ? 0 : 1);
                drawGlow (g, glow[i < n / 2 ? 0 : 1], p, 6.0f + 7.0f * amt, c.withAlpha (0.35f + 0.45f * amt));
                g.setOpacity (1.0f);
                g.setColour (white.withAlpha (0.55f + 0.4f * amt));
                g.fillEllipse (p.x - 1.6f, p.y - 1.6f, 3.2f, 3.2f);
            }

            // the send: flock -> poly, marching, brighter with the amount
            if (poly != nullptr)
            {
                const auto b = Links::centreOf (poly) - parallax;
                const auto d = b - centroid;
                const float len = d.getDistanceFromOrigin();
                if (len > 50.0f)
                {
                    const auto u = d / len;
                    const auto s0 = centroid + u * 10.0f, s1 = b - u * 38.0f;
                    const float segLen = (s1 - s0).getDistanceFromOrigin();
                    Path dp;
                    for (float t = std::fmod (march, 9.0f); t < segLen; t += 9.0f)
                        dp.addLineSegment ({ s0 + u * t, s0 + u * jmin (segLen, t + 4.0f) }, 1.1f);
                    g.setColour (pal.mote.withAlpha (0.12f + 0.55f * amt));
                    g.fillPath (dp);
                    drawGlow (g, sendGlow, centroid, 16.0f + 10.0f * amt, pal.mote.withAlpha (0.2f + 0.4f * amt));
                }
            }
        }
    };

    // film grain, faint scan lines and a vignette over everything
    struct Grain : public Component
    {
        std::vector<Image> tiles;
        Image vignette;
        int frame = 0;
        Grain()
        {
            setInterceptsMouseClicks (false, false);
            vignette = Image (Image::ARGB, (int) W / 2, (int) H / 2, true, SoftwareImageType());
            {
                Graphics g (vignette);
                g.addTransform (AffineTransform::scale (0.5f));
                g.setGradientFill (ColourGradient (Colours::transparentBlack, W * 0.5f, H * 0.45f, Colours::black.withAlpha (0.55f), 0.0f, 0.0f, true));
                g.fillRect (Rectangle<float> (W, H));
            }
            Random rng (0x6a1);
            for (int k = 0; k < 4; ++k)
            {
                Image im (Image::ARGB, 320, 320, true);
                for (int y = 0; y < 320; ++y)
                    for (int x = 0; x < 320; ++x)
                    {
                        const float n = rng.nextFloat();
                        if (n > 0.55f) im.setPixelAt (x, y, Colours::white.withAlpha (0.05f * (n - 0.55f) / 0.45f));
                        else if (n < 0.3f) im.setPixelAt (x, y, Colours::black.withAlpha (0.10f * (0.3f - n) / 0.3f));
                    }
                tiles.push_back (SoftwareImageType().convert (im));
            }
        }
        void advance() { frame = (frame + 1) % (int) tiles.size(); repaint(); }
        void paint (Graphics& g) override
        {
            g.setFillType (FillType (tiles[(size_t) frame], AffineTransform::scale (0.5f).translated ((float) ((frame * 37) % 160), (float) ((frame * 71) % 160))));
            g.fillRect (getLocalBounds());
            g.setColour (Colours::white.withAlpha (0.025f));
            for (float y = 40.0f; y < (float) getHeight(); y += 160.0f) g.fillRect (0.0f, y, (float) getWidth(), 1.0f);
            g.setOpacity (1.0f);
            g.drawImage (vignette, getLocalBounds().toFloat());
        }
    };

    // thin HUD title
    struct Title : public Component
    {
        float blurRadius = 0.0f;
        Title() { setInterceptsMouseClicks (false, false); setCachedComponentImage (new BlurCache (*this, blurRadius)); }
        void paint (Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat();
            const auto f = Font (FontOptions().withName ("Helvetica Neue").withStyle ("UltraLight").withHeight (27.0f).withKerningFactor (0.4f));
            g.setFont (f);
            g.setColour (white.withAlpha (0.25f));
            g.drawText ("CHLOAELIS", r.withTrimmedBottom (14).translated (0, 1), Justification::centred, false);
            g.setColour (white);
            g.drawText ("CHLOAELIS", r.withTrimmedBottom (14), Justification::centred, false);
        }
    };

    //==========================================================================
    struct Content : public Component
    {
        ChloaelisProcessor& proc;
        Backdrop backdrop;
        Bokeh bokeh;
        Hologram holo { proc };
        FlockView flockView { proc };
        Links links;
        Callouts callouts;
        Title title;
        Grain grain;
        OwnedArray<Node> nodes;
        OwnedArray<Knob> knobs;
        OwnedArray<AudioProcessorValueTreeState::SliderAttachment> sAtt;
        OwnedArray<Cell> cells;
        OwnedArray<Seg> segs;
        OwnedArray<AudioProcessorValueTreeState::ButtonAttachment> bAtt;
        OwnedArray<FilterPlot> fplots;
        OwnedArray<TapeStrip> strips;
        OwnedArray<Label> labels;
        std::vector<std::tuple<Label*, RangedAudioParameter*, Knob*>> readouts;
        Label* status[2] {};
        Node* delayHead[2] {};
        Node* tapeNode[2] {};
        Point<float> focusP { W * 0.5f, H * 0.5f }, pivot { W * 0.5f, H * 0.5f };
        Node* sticky = nullptr;
        int stickyOut = 0;
        float focusTight = 0.0f;
        float focusY = H * 0.5f, forcedFocus = -1.0f, heldFocus = -1.0f;
        Point<float> mouse { W * 0.5f, H * 0.5f }, forcedMouse { -1.0f, -1.0f };
        float yaw = 0.0f, pitch = 0.0f;       // the camera leans toward the cursor's side
        float warmth[2] {}, backdropWarmth = 0.0f;
        std::vector<Node*> depthOrder;
        int frame = 0;
        Random rng { 0xce115 };
        // (no tooltip window: the user doesn't want pop-up boxes)

        Knob* knob (Component& parent, Point<float> centre, float d, const String& id, const String& tip, int side)
        {
            auto* k = knobs.add (new Knob());
            k->side = side;
            parent.addAndMakeVisible (k);
            sAtt.add (new AudioProcessorValueTreeState::SliderAttachment (proc.apvts, id, *k));
            auto* prm = proc.apvts.getParameter (id);
            k->setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
            k->setTooltip (tip);
            k->setBounds (Rectangle<float> (d + 14, d + 14).withCentre (centre).toNearestInt());
            return k;
        }
        Label* label (Component& parent, Rectangle<float> r, const Font& f, Colour col)
        {
            auto* l = labels.add (new DiffLabel());
            l->setFont (f);
            l->setColour (Label::textColourId, col);
            l->setJustificationType (Justification::centred);
            l->setInterceptsMouseClicks (false, false);
            l->setBorderSize ({});
            l->setMinimumHorizontalScale (0.7f);
            parent.addAndMakeVisible (l);
            l->setBounds (r.toNearestInt());
            return l;
        }
        Node* node (Point<float> at, float w, float h, float angle, bool cellBg, int side)
        {
            auto* n = nodes.add (new Node (at, w, h, angle, cellBg, rng.nextFloat() * 6.28f, -0.6f + rng.nextFloat() * 1.2f));
            n->side = side;
            addAndMakeVisible (n);
            return n;
        }
        // a knob cell: name, dial, value
        Node* knobCell (const String& id, int side, Point<float> at, float angle, const char* name, const String& tip, Knob** out = nullptr)
        {
            auto* n = node (at, 62, 104, angle, true, side);
            auto* k = knob (*n, n->c().translated (0, -2), 44.0f, id, tip, side);
            if (out != nullptr) *out = k;
            k->setTooltip (String (name) + ": " + tip);
            const String nm0 (name);
            const int style = nm0 == "Cutoff" ? 2 : (nm0 == "Depth" || nm0 == "Source" || nm0 == "Feedback" || nm0 == "Flock") ? 1 : 0;
            if (style != 0)
            {
                // the whole cell is the control: a liquid vial or a sliding collar
                k->setBounds (n->getLocalBounds());
                k->vialStyle = style;
                k->vial = Rectangle<float> (62, 104).withCentre (n->c());
                k->vialAngle = angle;
                k->view = &n->hollow;
                k->blur = &n->blurRadius;
                n->vial = true;
            }
            auto* nm = label (*n, Rectangle<float> (40, 24).withCentre (n->c().translated (0, -35)), greek (20.0f), white);
            nm->setText (symbolFor (name), dontSendNotification);
            auto* l = label (*n, Rectangle<float> (76, 13).withCentre (n->c().translated (0, 34)), hud (9.5f, true), white);
            readouts.push_back ({ l, proc.apvts.getParameter (id), k });
            return n;
        }
        Node* knobCell (int side, int param, Point<float> at, float angle, const char* name, const String& tip, Knob** out = nullptr)
        {
            return knobCell (chl::pid (side, param), side, at, angle, name, tip, out);
        }
        // an LFO cell: rate and depth as two small dials
        Node* lfoCell (int side, int dest, int rateP, int depthP, Point<float> at, float angle, const char* name)
        {
            auto* n = node (at, 112, 76, angle, true, side);
            auto* nm = label (*n, Rectangle<float> (110, 16).withCentre (n->c().translated (0, -27)), greek (15.0f), white);
            nm->setText (String::fromUTF8 ("\xe2\x88\xbf ") + symbolFor (name), dontSendNotification);   // ∿ = LFO, then its target
            auto* kr = knob (*n, n->c().translated (-24, 0), 26.0f, chl::pid (side, rateP), String ("triangle LFO rate (to ") + name + ")", side);
            auto* kd = knob (*n, n->c().translated (24, 0), 26.0f, chl::pid (side, depthP), String ("LFO depth: sweeps ") + name + " upwards from where its knob is", side);
            kr->mini = kd->mini = true;
            kr->setTooltip (String ("LFO rate (") + name + "): " + kr->getTooltip());
            kd->setTooltip (String ("LFO depth (") + name + "): " + kd->getTooltip());
            kd->vialStyle = 1;   // depth as a small liquid vial
            kd->vial = Rectangle<float> (18, 38).withCentre (kd->getLocalBounds().toFloat().getCentre());
            kd->vialAngle = angle;
            kd->view = &n->hollow;
            kd->blur = &n->blurRadius;

            kd->lfo = &proc.lfoValue[dest][(size_t) side];
            for (auto [dx, p] : { std::pair<float, int> { -24.0f, rateP }, { 24.0f, depthP } })
            {
                auto* l = label (*n, Rectangle<float> (52, 11).withCentre (n->c().translated (dx, 27)), hud (8.0f, true), white);
                readouts.push_back ({ l, proc.apvts.getParameter (chl::pid (side, p)), p == rateP ? kr : kd });
            }
            return n;
        }
        Node* buttonCell (Cell* cl, Point<float> at, float w, const String& id, const String& tip, int side)
        {
            auto* n = node (at, w, 32, 0.0f, false, side);
            cells.add (cl);
            n->place (cl, {}, w, 32);
            bAtt.add (new AudioProcessorValueTreeState::ButtonAttachment (proc.apvts, id, *cl));
            cl->setTooltip (tip);
            return n;
        }
        Node* segCell (const String& id, StringArray opts, Point<float> at, float w, const String& tip, int side)
        {
            auto* n = node (at, w, 32, 0.0f, false, side);
            auto* sg = segs.add (new Seg (*proc.apvts.getParameter (id), std::move (opts), side));
            n->place (sg, {}, w, 32);
            sg->setTooltip (tip);
            lastSeg = sg;
            return n;
        }
        Seg* lastSeg = nullptr;
        std::function<void()> onFullscreen;
        HelpOverlay help;
        VesselView* vessel = nullptr;
        std::unique_ptr<VesselView> vesselOwner;
        void link (Component* a, Component* b, int side) { links.links.push_back ({ a, b, side }); }

        explicit Content (ChloaelisProcessor& p) : proc (p)
        {
            updatePalette (0.0f, 0.0f);
            setSize ((int) W, (int) H);
            // (the backdrop, bokeh and grain are children of the editor, so they can fill a
            //  window of any shape: see ChloaelisEditor::resized)
            addAndMakeVisible (holo); holo.setBounds (getLocalBounds());
            addAndMakeVisible (flockView); flockView.setBounds (getLocalBounds());
            addAndMakeVisible (links); links.setBounds (getLocalBounds());
            addAndMakeVisible (title); title.setBounds ((int) W / 2 - 170, 2, 340, 52);

            Node* cutNode[2] {};
            Node* inNode[2] {};
            Node* timeNode[2] {};
            Node* outNode[2] {};

            for (int s = 0; s < 2; ++s)
            {
                auto P = [s] (float x, float y) { return Point<float> (s == 0 ? x : W - x, y); };
                const float fl = s == 0 ? 1.0f : -1.0f;
                const String sn = s == 0 ? "L" : "R";
                Knob* k = nullptr;

                // input
                auto* nIn   = knobCell (s, chl::inAtt, P (118, 168), -0.30f * fl, "Input", "input level");
                nIn->title = "Input";
                inNode[s] = nIn;

                // filter
                auto* nCut  = knobCell (s, chl::cutoff, P (232, 262), -0.2f * fl, "Cutoff", "filter cutoff (the Lorenz attractor moves it when its depth is up)", &k);
                k->mod = &proc.paramNorm[(size_t) s][chl::cutoff];
                nCut->title = "Filter";
                auto* nPlot = node (P (372, 214), 150, 44, 0.0f, false, s);
                fplots.add (nPlot->place (new FilterPlot (proc, s), {}, 150, 44));
                auto* nRes  = knobCell (s, chl::res, P (306, 336), 0.3f * fl, "Reso", "filter resonance");
                auto* nMorph= knobCell (s, chl::morph, P (404, 322), -0.25f * fl, "Mode", "filter mode: low pass > band pass > high pass > notch", &k);
                k->mod = &proc.paramNorm[(size_t) s][chl::morph];
                auto* nLM   = lfoCell (s, 0, chl::lmRate, chl::lmDepth, P (414, 432), 0.12f * fl, "Mode");

                // tube
                auto* nDrv  = knobCell (s, chl::drive, P (92, 312), 0.2f * fl, "Drive", "tube drive", &k);
                k->mod = &proc.paramNorm[(size_t) s][chl::drive];
                nDrv->title = "Tube";
                auto* nTOn  = buttonCell (new Cell ("Tube On", "", s), P (150, 400), 96, chl::tubeOnId (s), "tube distortion on / off", s);
                auto* nLD   = lfoCell (s, 2, chl::ldRate, chl::ldDepth, P (96, 492), -0.1f * fl, "Drive");
                // tube type: I 12AX7 (default), II EL84, III 6L6 with sag, IV starved
                auto* nTT   = segCell (chl::tubeTypeId (s), { "I", "II", "III", "IV" }, P (218, 446), 128, {}, s);
                link (nDrv, nTT, s);

                // delay
                auto* nTime = knobCell (s, chl::time, P (82, 634), -0.25f * fl, "Time", "delay time", &k);
                k->mod = &proc.paramNorm[(size_t) s][chl::time];
                nTime->title = "Delay"; delayHead[s] = nTime;
                auto* nType = segCell (chl::delayTypeId (s), { "Tape", "PT2399", "BBD" }, P (252, 526), 196,
                                       "delay engine.  TAPE: an ageing tape loop (hiss, warble, wear; refresh for new tape).  PT2399: the chip, gritty at long times.  BBD: bucket brigade, crunchy and dark at long times", s);
                auto* nReg  = knobCell (s, chl::regen, P (188, 654), 0.2f * fl, "Regen", "delay regeneration (past ~90% it self-oscillates)");
                auto* nMix  = knobCell (s, chl::wet, P (300, 642), -0.3f * fl, "Mix", "delay dry / wet");
                auto* nSlew = segCell (chl::timeModeId (s), { "Slew", "Fade" }, P (416, 552), 118,
                                       "time changes.  SLEW: the clock / tape speed glides, so echoes bend in pitch.  FADE: the new time crossfades in, no pitch change", s);
                auto* nRnd  = knobCell (s, chl::rnd, P (398, 646), 0.22f * fl, "Rand", "random freeze: the chance that the buffer gets held for a while, at random moments");
                auto* nLT   = lfoCell (s, 1, chl::ltRate, chl::ltDepth, P (112, 752), 0.08f * fl, "Time");
                auto* nHpf  = buttonCell (new Cell ("HPF", "", s), P (238, 748), 70, chl::hpfId (s), "high pass filter in the delay feedback path", s);
                auto* nFrz  = buttonCell (new Cell ("Freeze", "", s), P (330, 760), 84, chl::freezeId (s), "freeze the delay buffer (on tape, a held loop slowly disintegrates as the tape wears)", s);
                status[s] = label (*nFrz, Rectangle<float> (150, 12).withCentre (nFrz->c().translated (0, 28)), hud (8.5f, true), white.withAlpha (0.6f));
                auto* nTape = node (P (430, 744), 104, 54, 0.0f, false, s);
                tapeNode[s] = nTape;
                auto* rf = new Cell ("Refresh", "", s);
                rf->setClickingTogglesState (false);
                rf->onClick = [this, s] { proc.refreshTape[(size_t) s] = true; };
                rf->setTooltip ("put on a fresh tape: the wear (dropouts, dullness, hiss, warble) goes back to new");
                cells.add (rf);
                nTape->place (rf, { 0, -10 }, 100, 32);
                strips.add (nTape->place (new TapeStrip (proc, s), { 0, 16 }, 96, 24));

                // out
                auto* nOut  = knobCell (s, chl::out, P (520, 742), 0.25f * fl, "Out", "output level (the cells glow with it: accent, then warm white when hot, red at the top)");
                
                link (nIn, nCut, s); link (nCut, nPlot, s); link (nCut, nRes, s); link (nRes, nMorph, s); link (nMorph, nLM, s);
                link (nCut, nDrv, s); link (nDrv, nTOn, s); link (nDrv, nLD, s);
                link (nTOn, nType, s); link (nType, nTime, s); link (nTime, nReg, s); link (nTime, nLT, s);
                link (nReg, nMix, s); link (nReg, nHpf, s); link (nHpf, nFrz, s); link (nMix, nSlew, s); link (nMix, nRnd, s);
                link (nRnd, nFrz, s); link (nFrz, nTape, s); link (nMix, nOut, s);
                cutNode[s] = nCut; timeNode[s] = nTime; outNode[s] = nOut;
            }

            // the shared middle: Lorenz attractor -> both cutoffs, poly width -> both delays, order
            auto* nSp = knobCell (chl::lzSpeedId, -1, { W * 0.5f - 84, 178 }, -0.18f, "Speed", "how fast the attractor travels: slow drifts to fast flutter");
            auto* nCh = knobCell (chl::lzChaosId, -1, { W * 0.5f, 214 }, 0.1f, "Chaos", "rho: low = it barely holds a lobe and wanders gently; high = wild, with odd periodic windows");
            auto* nSh = knobCell (chl::lzShapeId, -1, { W * 0.5f + 84, 178 }, 0.2f, "Shape", "beta: reshapes the orbit (thin and twitchy vs. fat and lazy)");
            auto* nDp = knobCell (chl::lzDepthId, -1, { W * 0.5f, 352 }, -0.12f, "Depth", "how far the attractor moves the cutoffs (up to +/- 3.5 octaves)");
            auto* nOrd = segCell (chl::orderId, { String::fromUTF8 ("Filter \xe2\x80\xba Dly"), String::fromUTF8 ("Dly \xe2\x80\xba Filter") }, { W * 0.5f, 548 }, 190,
                                  "order: filter > tube > delay, or delay > tube > filter", -1);
            auto* nW  = knobCell (chl::widthId, -1, { W * 0.5f, 636 }, 0.15f, "Width", "poly: fades in up to 7 more delay taps per side, one after another, each at its own offset time. all the way up = 8 taps, a reverb-like cloud. all the way down = off");
            nW->title = "Poly";
            auto* nFl = knobCell (chl::flockId, -1, { W * 0.5f, 752 }, -0.1f, "Flock",
                                  "flock: a boids simulation moves each poly tap's delay time (up to ~0.9 octave). the taps drift, cluster and scatter together");
            link (nW, nFl, -1);
            flockView.poly = nW;

            nSp->title = "Lorenz";

            // the built-in recording (which version, and its level) and the master feedback
            auto* nSrcSel = segCell (chl::srcSelId, { String::fromUTF8 ("\xe2\x88\x9e"), String::fromUTF8 ("\xe2\x89\x88") }, { 300, 96 }, 92,
                                     String::fromUTF8 ("built-in recording.  \xe2\x88\x9e: baserecording1, a long loop from its middle (3:36 to 7:25).  \xe2\x89\x88: the hydrophone recording (june 2026, 3), looping"), -1);
            lastSeg->fontH = 15.0f;    // the symbols need to be legible

            // desktop (standalone) only: the vessel you pour audio into, and a fullscreen toggle
            if (proc.isStandalone())
            {
                auto* nV = node ({ W - 300, 104 }, 70, 130, 0.0f, true, -1);
                nV->vial = true;     // the vessel draws its own glass
                vessel = new VesselView (proc);
                vesselOwner.reset (vessel);
                nV->addAndMakeVisible (vessel);
                vessel->setBounds (nV->getLocalBounds());
                vessel->body = Rectangle<float> (70, 130).withCentre (nV->c());
                vessel->view = &nV->hollow;
                nV->title = "Vessel";
                link (nV, inNode[0], 0); link (nV, inNode[1], 1);

                auto* nFs = node ({ W - 58, 72 }, 58, 30, 0.0f, false, -1);
                auto* fs = new Cell (String::fromUTF8 ("\xe2\xa4\xa2"), "", -1);
                fs->setClickingTogglesState (false);
                fs->onClick = [this] { if (onFullscreen) onFullscreen(); };
                cells.add (fs);
                nFs->place (fs, {}, 54, 28);
            }

            // reset: every control back to its default (the tape's wear is left alone)
            {
                auto* nRst = node ({ W - 58, 36 }, 58, 30, 0.0f, false, -1);
                auto* rs = new Cell (String::fromUTF8 ("\xe2\x86\xba"), "", -1);
                rs->setClickingTogglesState (false);
                rs->onClick = [this]
                {
                    for (auto* prm : proc.getParameters())
                        if (auto* rp = dynamic_cast<RangedAudioParameter*> (prm))
                        {
                            rp->beginChangeGesture();
                            rp->setValueNotifyingHost (rp->getDefaultValue());
                            rp->endChangeGesture();
                        }
                };
                cells.add (rs);
                nRst->place (rs, {}, 54, 28);
            }
            auto* nSrc = knobCell (chl::srcLevelId, -1, { 410, 116 }, -0.12f, "Source", "the built-in recording's level: fade it in and out (left into the left strip, right into the right)");
            nSrc->title = "Source";
            auto* nFb = knobCell (chl::masterFbId, -1, { W - 410, 116 }, 0.12f, "Feedback", "master feedback: each side's output back into its own input. past ~90% it can sing on its own");
            nFb->title = "Feedback";
            link (nSrcSel, nSrc, -1);
            link (nSrc, inNode[0], 0); link (nSrc, inNode[1], 1);
            link (nFb, inNode[0], 0); link (nFb, inNode[1], 1);
            link (nDp, cutNode[0], 0); link (nDp, cutNode[1], 1);
            holo.cut[0] = cutNode[0]; holo.cut[1] = cutNode[1];
            link (nSp, nCh, -1); link (nCh, nSh, -1); link (nCh, nDp, -1);
            link (nW, timeNode[0], 0); link (nW, timeNode[1], 1); link (nOrd, nW, -1);
            link (nW, outNode[0], 0); link (nW, outNode[1], 1);

            links.nodes = std::vector<Node*> (nodes.begin(), nodes.end());
            links.blurAt = [this] (Point<float> at) { return blurFor (at); };
            callouts.blurAt = links.blurAt;
            for (auto* n : nodes) callouts.nodes.push_back (n);
            addAndMakeVisible (callouts); callouts.setBounds (getLocalBounds());

            // the help screen (hidden until the ? cell is clicked)
            help.desktop = proc.isStandalone();
            addChildComponent (help); help.setBounds (getLocalBounds());
            {
                auto* nHelp = node ({ W - 58, proc.isStandalone() ? 108.0f : 72.0f }, 58, 30, 0.0f, false, -1);
                auto* hb = new Cell ("?", "", -1);
                hb->setClickingTogglesState (false);
                hb->onClick = [this] { help.setVisible (! help.isVisible()); help.toFront (false); };
                cells.add (hb);
                nHelp->place (hb, {}, 54, 28);
            }
            tick();
        }

        void paint (Graphics&) override { ++chloaelisDbgFrames; }   // called once per rendered frame

        // shallow depth of field: sharp inside a small circle round the focus, blurring fast outside
        float blurFor (Point<float> p) const
        {
            const float d = p.getDistanceFrom (focusP);
            const float tight = jlimit (0.0f, 1.0f, focusTight);
            const float sharpR = jmap (tight, 300.0f, 120.0f), ramp = jmap (tight, 70.0f, 22.0f);
            return jlimit (0.0f, 8.0f, (d - sharpR) / ramp);
        }

        void tick()
        {
            const double t = now();
            ++frame;
            fitBudget = 6;
            blurMsLeft = 4.0;
            // input level -> warmth: quick to warm, slow to cool, like a meter's ballistics
            for (int s = 0; s < 2; ++s)
            {
                const float target = jlimit (0.0f, 1.0f, (levelDb (proc.inLevel[(size_t) s].load()) + 42.0f) / 36.0f);
                warmth[s] += (target - warmth[s]) * (target > warmth[s] ? 0.18f : 0.035f);
            }
            updatePalette (warmth[0], warmth[1]);
            const bool forced = forcedMouse.x >= 0.0f;
            const bool over = forced || isMouseOver (true);
            const bool dragging = ! forced && ModifierKeys::currentModifiers.isAnyMouseButtonDown();
            if (forced)       mouse = forcedMouse;
            else if (over)    mouse = getMouseXYRelative().toFloat();
            const float ff = forcedFocus >= 0.0f ? forcedFocus : heldFocus;
            const Point<float> par ((mouse.x - W * 0.5f) / W, (mouse.y - H * 0.5f) / H);

            // which parameter (cell) the cursor is on, if any. Sticky: a hovered cell lifts and
            // leans, which can slide it out from under a cursor resting near its edge; so it
            // stays hovered until the cursor has been clearly outside a wider zone for a moment.
            Node* hovered = nullptr;
            if (! forced)
            {
                auto inZone = [this, over] (Node* n, float margin)
                {
                    auto lp = mouse.transformedBy (n->getTransform().inverted()) - n->c();
                    const float ca = std::cos (-n->angle), sa = std::sin (-n->angle);
                    const float lx = lp.x * ca - lp.y * sa, ly = lp.x * sa + lp.y * ca;
                    return over && std::abs (lx) <= n->cw * 0.5f + margin && std::abs (ly) <= n->ch * 0.5f + margin;
                };
                Node* hit = nullptr;
                for (auto* n : nodes)
                    if (n->isMouseOverOrDragging (true)) { hit = n; break; }
                if (sticky != nullptr && hit != nullptr && hit != sticky) sticky = hit;     // moved onto another cell
                if (sticky != nullptr)
                {
                    if (inZone (sticky, 30.0f) || sticky->isMouseButtonDown (true)) stickyOut = 0;
                    else if (++stickyOut > 8) sticky = nullptr;                                // ~130 ms clearly off it
                }
                if (sticky == nullptr && hit != nullptr) { sticky = hit; stickyOut = 0; }
                hovered = sticky;
            }
            if (forced && ff < 0.0f)   // the test hook: treat the nearest cell as hovered
            {
                float best = 70.0f;
                for (auto* n : nodes) if (n->home.getDistanceFrom (mouse) < best) { best = n->home.getDistanceFrom (mouse); hovered = n; }
            }

            // Depth of field. Focus is a small circle that glides to the parameter under the
            // cursor: that cell and its close neighbours are sharp, everything else falls away
            // into blur quickly. Off a cell it widens a little; outside the window, wider still.
            {
                Point<float> target (W * 0.5f, H * 0.5f);
                float tTight = 0.0f;
                if (ff >= 0.0f)        { target = { mouse.x, ff }; tTight = 0.5f; }
                else if (hovered)      { target = hovered->home; tTight = 1.0f; }
                else if (over)         { target = mouse; tTight = 0.5f; }
                focusP += (target - focusP) * 0.08f;
                focusTight += (tTight - focusTight) * 0.06f;
                focusY = focusP.y;
            }

            // Perspective camera. The field is a plane in 3D that pivots about the cursor: the
            // cursor's side stays put (or comes slightly nearer), the far side swings way back,
            // shrinking toward the vanishing point and skewing with the plane. Each cell gets
            // the true local perspective of its spot on the plane (scale, squash and shear).
            // Everything eases, so it floats rather than snaps. While a dial is being dragged
            // the camera holds still, so the thing under your hand doesn't slide away.
            if (! dragging)
            {
                const float tYaw   = over ? jlimit (-1.0f, 1.0f, (mouse.x - W * 0.5f) / (W * 0.5f)) : 0.0f;
                const float tPitch = over ? jlimit (-1.0f, 1.0f, (mouse.y - H * 0.5f) / (H * 0.5f)) : 0.0f;
                yaw   += (tYaw - yaw) * 0.05f;
                pitch += (tPitch - pitch) * 0.05f;
                pivot += ((over ? mouse : Point<float> (W * 0.5f, H * 0.5f)) - pivot) * 0.05f;
            }
            constexpr float focal = 1100.0f, maxYaw = 0.11f, maxPitch = 0.075f, liftZ = 80.0f, layerZ = 30.0f;
            const float reach = 190.0f - 80.0f * focusTight;
            const float yb = -yaw * maxYaw, pc = pitch * maxPitch;
            const float cyb = std::cos (yb), syb = std::sin (yb), cpc = std::cos (pc), spc = std::sin (pc);
            // world (design px, z toward the viewer) -> screen, and the depth it ends up at
            auto project = [&] (Point<float> w, float zl, float& zOut)
            {
                const float X = w.x - pivot.x, Y = w.y - pivot.y;
                const float x1 = X * cyb + zl * syb, z1 = -X * syb + zl * cyb;
                const float y2 = Y * cpc - z1 * spc, z2 = Y * spc + z1 * cpc;
                zOut = jmin (z2, 320.0f);
                const float sc = focal / (focal - zOut);
                return pivot + Point<float> (x1, y2) * sc;
            };

            for (auto* n : nodes)
            {
                const bool held = n == hovered || n->isMouseButtonDown (true);
                if (dragging && n->isMouseButtonDown (true)) continue;      // pinned under the hand

                const float d = n->home.getDistanceFrom (mouse);
                const float tLift = held ? 1.0f : (over ? std::exp (-(d * d) / (2.0f * reach * reach)) : 0.0f);
                if (! dragging) n->lift += (tLift - n->lift) * (tLift > n->lift ? 0.09f : 0.05f);

                const float dx = held ? 0.0f : 3.0f * (float) std::sin (t * 0.6 + n->phase);
                const float dy = held ? 0.0f : 4.5f * (float) std::sin (t * 0.8 + n->phase * 1.7);
                const float zl = layerZ * n->depth + liftZ * n->lift;
                const auto w0 = n->home + Point<float> (dx - par.x * 18.0f * n->depth, dy - par.y * 12.0f * n->depth);
                float z, zx, zy;
                const auto p0 = project (w0, zl, z);
                const auto px = project (w0 + Point<float> (1.0f, 0.0f), zl, zx);
                const auto py = project (w0 + Point<float> (0.0f, 1.0f), zl, zy);
                // the plane's local perspective at this cell: columns = where one px of x / y goes
                const float j00 = px.x - p0.x, j10 = px.y - p0.y, j01 = py.x - p0.x, j11 = py.y - p0.y;
                const float s = std::sqrt (jmax (0.01f, std::abs (j00 * j11 - j01 * j10)));
                n->z = z;
                n->persp = s;
                n->cache->zoom = s;

                // keep it inside the window; lifted cells lean gently toward the cursor
                auto to = p0 + (mouse - n->home) * (0.03f * n->lift);
                to = { jlimit (n->cw * s * 0.5f + 6.0f, W - n->cw * s * 0.5f - 6.0f, to.x), jlimit (n->ch * s * 0.5f + 6.0f, H - n->ch * s * 0.5f - 6.0f, to.y) };
                const auto c = n->getBounds().toFloat().getCentre();
                n->moveTo (AffineTransform (j00, j01, to.x - (j00 * c.x + j01 * c.y),
                                            j10, j11, to.y - (j10 * c.x + j11 * c.y)));

                // focus circle, plus depth: whatever has swung back into the distance softens
                const float depthBlur = jmax (0.0f, -z - 15.0f) / 50.0f;
                n->blurRadius = jlimit (0.0f, 9.0f, (blurFor (n->home) + depthBlur) * (1.0f - (held ? 0.9f : 0.25f) * n->lift));
            }

            // the glass highlights slide with the camera: quantised, and at most 2 cells re-render a frame
            {
                int budget = 2;
                const int count = nodes.size();
                for (int j = 0; j < count && budget > 0; ++j)
                {
                    auto* n = nodes[(frame * 3 + j) % count];
                    if (! n->drawCell || n->blurRadius >= 2.5f) continue;   // a blurred cell's highlight can't be seen to move
                    const float ca = std::cos (-n->angle), sa = std::sin (-n->angle);
                    const float lx = yaw * ca - pitch * sa, ly = yaw * sa + pitch * ca;
                    const Point<float> q (std::round (lx * 3.2f * 2.0f) * 0.5f, std::round (ly * 4.5f * 2.0f) * 0.5f);
                    if (q != n->hollow) { n->hollow = q; n->repaint(); --budget; }
                }
            }

            // nearer cells draw over farther ones (re-sorted a few times a second)
            if (frame % 6 == 0 && ! dragging)
            {
                auto order = std::vector<Node*> (nodes.begin(), nodes.end());
                std::stable_sort (order.begin(), order.end(), [] (Node* a, Node* b) { return a->z < b->z; });
                if (order != depthOrder)
                {
                    for (auto* n : order) n->toBehind (&callouts);
                    depthOrder = std::move (order);
                }
            }
            title.blurRadius = blurFor ({ W * 0.5f, 36.0f });
            bokeh.parallax = { -par.x * 40.0f, -par.y * 26.0f };
            holo.parallax = { -par.x * 22.0f, -par.y * 14.0f };
            holo.camYaw = yaw; holo.camPitch = pitch;
            holo.advance (1.0f / 60.0f);
            flockView.parallax = { -par.x * 16.0f, -par.y * 10.0f };
            flockView.advance (1.0f / 60.0f);
            flockView.repaint();

            // output levels -> colour (fast up, slower down)
            for (int s = 0; s < 2; ++s)
            {
                const float d = levelDb (proc.outLevel[(size_t) s].load());
                links.db[s] = d > links.db[s] ? d : links.db[s] + (d - links.db[s]) * 0.08f;
            }

            for (auto* k : knobs) k->poll();
            if (vessel != nullptr) vessel->poll (frame);
            for (auto* f : fplots) f->update();
            for (auto* st : strips) st->update();
            for (auto& [l, prm, kn] : readouts)
            {
                const String full = prm->getCurrentValueAsText() + prm->getLabel();
                const String s = (kn != nullptr && kn->isMouseOverOrDragging() ? full : compactValue (prm->getCurrentValueAsText())).toUpperCase();
                if (l->getText() != s) l->setText (s, dontSendNotification);
            }
            for (int s = 0; s < 2; ++s)
            {
                const bool fr = proc.frozen[(size_t) s].load() > 0.5f, rnd = proc.randHeld[(size_t) s].load() > 0.5f;
                const bool manual = proc.apvts.getRawParameterValue (chl::freezeId (s))->load() > 0.5f;
                const String txtS = manual ? "BUFFER HELD" : (rnd ? "RANDOM HOLD" : (fr ? "BUFFER HELD" : "BUFFER LIVE"));
                if (status[s]->getText() != txtS)
                {
                    status[s]->setText (txtS, dontSendNotification);
                    status[s]->setColour (Label::textColourId, fr ? acc (s) : white.withAlpha (0.55f));
                }
                const int ty = jlimit (0, 2, (int) std::lround (proc.apvts.getRawParameterValue (chl::delayTypeId (s))->load()));
                const float ta = ty == 0 ? 1.0f : 0.45f;
                if (std::abs (tapeNode[s]->getAlpha() - ta) > 0.01f) tapeNode[s]->setAlpha (ta);
            }

            // the palette follows the level: the backdrop re-renders when the warmth has moved
            // (at most 7.5 times a second), the cells a few at a time, so no frame redraws
            // everything. The glows under the cells and the links follow the level every frame.
            const float wNow = jmax (warmth[0], warmth[1]);
            if (frame % 8 == 0 && std::abs (wNow - backdropWarmth) > 0.02f) { backdropWarmth = wNow; backdrop.repaint(); }
            if (! nodes.isEmpty())
                for (int i = 0; i < 3; ++i)
                    nodes[(frame * 3 + i) % nodes.size()]->repaint();
            bokeh.repaint();
            holo.repaint();
            links.repaint();
            callouts.repaint();
            if (frame % 4 == 0) grain.advance();      // grain flickers at 15 fps, like film
        }
    };
}

//==============================================================================
ChloaelisEditor::ChloaelisEditor (ChloaelisProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    lnf = std::make_unique<HudLnF>();
    setLookAndFeel (lnf.get());
    content = std::make_unique<Content> (p);
    auto* c = static_cast<Content*> (content.get());
    addAndMakeVisible (c->backdrop);
    addAndMakeVisible (c->bokeh);
    addAndMakeVisible (*content);
    addAndMakeVisible (c->grain);
    c->onFullscreen = [this] { toggleFullScreen(); };

    setResizable (true, true);
    if (p.isStandalone())
        setResizeLimits (designW / 2, designH / 2, 16384, 16384);     // free shape: fullscreen works, the UI letterboxes
    else
    {
        setResizeLimits (designW / 2, designH / 2, designW * 2, designH * 2);
        if (auto* cons = getConstrainer())
            cons->setFixedAspectRatio ((double) designW / (double) designH);
    }
    setSize (designW, designH);
    gl.setComponentPaintingEnabled (true);
    gl.setContinuousRepainting (false);
    gl.attachTo (*this);
    startTimerHz (60);
}

ChloaelisEditor::~ChloaelisEditor()
{
    stopTimer();
    if (kiosk) toggleFullScreen();
    if (auto* w = hostWindow.getComponent()) w->removeKeyListener (this);
    gl.detach();
    content.reset();
    setLookAndFeel (nullptr);
}

void ChloaelisEditor::paint (Graphics& g) { g.fillAll (pal.field0); }

void ChloaelisEditor::resized()
{
    // the cells fit and centre; the field (backdrop, bokeh, grain) fills the whole window, so
    // any screen shape is covered edge to edge with no bars
    const float s = jmin ((float) getWidth() / (float) designW, (float) getHeight() / (float) designH);
    const float fw = (float) getWidth() / s, fh = (float) getHeight() / s;
    auto* c = static_cast<Content*> (content.get());
    for (Component* f : { (Component*) &c->backdrop, (Component*) &c->bokeh, (Component*) &c->grain })
    {
        f->setBounds (0, 0, (int) std::ceil (fw), (int) std::ceil (fh));
        f->setTransform (AffineTransform::scale (s));
    }
    content->setTransform (AffineTransform::scale (s).translated (((float) getWidth() - designW * s) * 0.5f, ((float) getHeight() - designH * s) * 0.5f));
}

void ChloaelisEditor::toggleFullScreen()
{
    auto* win = hostWindow.getComponent();
    if (win == nullptr) win = findParentComponentOfClass<DocumentWindow>();
    if (win == nullptr) return;
    auto& desk = Desktop::getInstance();
    auto setOptionsVisible = [win] (bool v)
    {
        for (auto* ch : win->getChildren())
            if (auto* b = dynamic_cast<Button*> (ch); b != nullptr && b->getButtonText() == "Options")
                b->setVisible (v);
    };
    if (! kiosk)
    {
        savedBounds = win->getBounds();
        savedTitleH = win->getTitleBarHeight();
        win->setTitleBarHeight (0);
        win->setResizable (false, false);
        setOptionsVisible (false);
        desk.setKioskModeComponent (win, false);     // hides the menu bar and the dock, fills the display
        kiosk = true;
    }
    else
    {
        desk.setKioskModeComponent (nullptr);
        win->setTitleBarHeight (savedTitleH);
        win->setResizable (true, false);
        setOptionsVisible (true);
        win->setBounds (savedBounds);
        kiosk = false;
    }
}

bool ChloaelisEditor::keyPressed (const KeyPress& key, Component*)
{
    if (key == KeyPress ('f', ModifierKeys::commandModifier, 0)) { toggleFullScreen(); return true; }
    if (key == KeyPress::escapeKey)
    {
        auto& h = static_cast<Content*> (content.get())->help;
        if (h.isVisible()) { h.setVisible (false); return true; }
        if (kiosk) { toggleFullScreen(); return true; }
    }
    return false;
}

void ChloaelisEditor::advanceFrames (int frames, float focusY)
{
    auto* c = static_cast<Content*> (content.get());
    c->forcedFocus = focusY;
    for (int i = 0; i < frames; ++i)
        c->tick();
    c->forcedFocus = -1.0f;
    fitBudget = 1 << 20;     // a test snapshot paints once after many ticks: let every cell catch up
    blurMsLeft = 1.0e9;
}

void ChloaelisEditor::holdMouseForTest (float x, float y)
{
    static_cast<Content*> (content.get())->forcedMouse = { x, y };
}

void ChloaelisEditor::showHelpForTest (bool show)
{
    static_cast<Content*> (content.get())->help.setVisible (show);
}

void ChloaelisEditor::holdFocusForTest (float y)
{
    static_cast<Content*> (content.get())->heldFocus = y;
}

void ChloaelisEditor::timerCallback()
{
    // standalone: once the window exists, listen for cmd+F / esc on it
    if (proc.isStandalone() && hostWindow == nullptr)
        if (auto* w = findParentComponentOfClass<DocumentWindow>())
        {
            hostWindow = w;
            w->addKeyListener (this);
        }
    static_cast<Content*> (content.get())->tick();
}
