#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_opengl/juce_opengl.h>
#include "PluginProcessor.h"

class ChloaelisEditor : public juce::AudioProcessorEditor, private juce::Timer, private juce::KeyListener
{
public:
    static constexpr int designW = 1200, designH = 820;

    explicit ChloaelisEditor (ChloaelisProcessor&);
    ~ChloaelisEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // advance the animation without the timer (used by the headless screenshot test)
    void advanceFrames (int frames, float focusY = -1.0f);
    void holdMouseForTest (float x, float y);   // pretend the cursor is here (design units); x < 0 releases it
    void showHelpForTest (bool show);
    void setDensityForTest (float d);   // the cells' size against their spacing (a preview of a tighter layout)
    void holdFocusForTest (float y);   // < 0 releases it   // focusY >= 0: hold the focus band there (design units)

private:
    void timerCallback() override;

    // standalone: true fullscreen (kiosk: no menu bar, no dock, no title bar), cmd+F / esc
    void toggleFullScreen();
    using juce::AudioProcessorEditor::keyPressed;
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;
    juce::Component::SafePointer<juce::DocumentWindow> hostWindow;
    bool kiosk = false;
    juce::Rectangle<int> savedBounds;
    int savedTitleH = 0;

    [[maybe_unused]] ChloaelisProcessor& proc;
    std::unique_ptr<juce::LookAndFeel> lnf;
    std::unique_ptr<juce::Component> content;
    juce::OpenGLContext gl;      // GPU compositing: the cached cells, sprites and grain are drawn as textures

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChloaelisEditor)
};
