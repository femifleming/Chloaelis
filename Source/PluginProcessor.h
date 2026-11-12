#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "DSP.h"
#include "Stream.h"

class ChloaelisProcessor : public juce::AudioProcessor
{
public:
    ChloaelisProcessor();
    ~ChloaelisProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Chloaelis"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // read by the UI
    std::array<std::array<std::atomic<float>, chl::numMod>, chl::numSides> paramNorm {};   // current (modulated) normalised values
    std::array<std::atomic<float>, chl::numSides> tubeGlow {}, inLevel {}, outLevel {}, wetLevel {}, frozen {}, randHeld {};
    std::array<std::atomic<float>, chl::numSides> tapeAge {};          // 0 = new tape, 1 = ruined
    std::array<std::atomic<float>, chl::numSides> lfoValue[3] {};       // per destination (mode, time, drive), 0..1
    std::atomic<float> lzX { 0.1f }, lzY { 0.0f }, lzZ { 20.0f };       // the shared attractor's state (for the display)
    std::array<std::atomic<float>, chl::numSides> lzOut {};           // what it sends to each cutoff (-1..1, before depth)
    std::array<std::atomic<float>, chl::dsp::Flock::n> flockX {}, flockY {};   // the boids (-1..1), for the display

    std::array<std::atomic<float>, chl::numSides> ksHz {};             // Karplus-Strong: the note each side is on now (Hz)
    std::atomic<int> uiTheme { 0 };                                    // 0 = the glass HUD, 1 = Brick (Windows 2000)

    // written by the UI
    std::array<std::atomic<bool>, chl::numSides> refreshTape {};

    // ---- the vessel (standalone only): a dropped audio file, looped between two rings, its
    //      left into the left strip and its right into the right
    struct VesselClip
    {
        juce::AudioBuffer<float> audio;     // stereo, at the file's own rate
        double rate = 44100.0;
        std::vector<float> peaks;           // for the display: max |x| per slice, oldest first
        juce::String name;
    };
    bool isStandalone() const { return wrapperType == wrapperType_Standalone; }
    bool loadVesselFile (const juce::File&, bool keepLoop = false);   // message thread
    std::shared_ptr<const VesselClip> vesselForUi() const { return vesselUi; }
    std::atomic<float> vesselStart { 0.0f }, vesselEnd { 1.0f }, vesselLevel { 0.0f }, vesselPos { 0.0f };
    juce::File vesselFile;

private:
    juce::AudioProcessorEditor* createEditor() override;

    void readTargets (float (&dst)[chl::numSides][chl::numMod]);

    // the last state it was closed in, restored when a new instance starts (a host session
    // that has its own saved state overrides it, as it should)
    static juce::File lastStateFile();
    void saveLastState();
    void loadLastState();
    juce::ValueTree stateWithExtras();
    void applyState (const juce::ValueTree&);
    void modulate (float (&dst)[chl::numSides][chl::numMod], int len);

    chl::dsp::ChloeVoice voices[chl::numSides];
    chl::dsp::Lorenz lorenz;
    juce::Random rng { 0xf2ee };

    std::atomic<float>* pRaw[chl::numSides][chl::numMod] {};
    std::atomic<float>* pHpf[chl::numSides] {};
    std::atomic<float>* pFreeze[chl::numSides] {};
    std::atomic<float>* pTubeOn[chl::numSides] {};
    std::atomic<float>* pType[chl::numSides] {};
    std::atomic<float>* pTMode[chl::numSides] {};
    std::atomic<float>* pTubeType[chl::numSides] {};
    std::atomic<float>* pKs[chl::numSides] {};
    std::atomic<float>* pKsScale = nullptr;
    float ksLastNorm[chl::numSides] { -1.0f, -1.0f };
    int ksLastScale = -1;
    chl::KsNote ksCur[chl::numSides];
    float ksTapRatio[chl::numSides][7] {};
    std::atomic<float>* pSrcSel = nullptr, *pSrcLevel = nullptr, *pMasterFb = nullptr, *pFlock = nullptr;
    chl::dsp::Flock flock;
    float flockCur = 0.0f;
    std::atomic<float>* pLzSpeed = nullptr, *pLzChaos = nullptr, *pLzShape = nullptr, *pLzDepth = nullptr, *pWidth = nullptr, *pOrder = nullptr;
    juce::NormalisableRange<float> ranges[chl::numMod];

    float cur[chl::numSides][chl::numMod] {};
    float widthCur = 0;
    float lfoPhase[chl::numSides][3] {};
    float rfTimer[chl::numSides] {};
    bool rfOn[chl::numSides] {};
    std::atomic<float> pendingAge[chl::numSides] { -1.0f, -1.0f };
    double sr = 48000.0;

    // the built-in recording: [0] the whole thing, [1] a long loop from the middle
    std::vector<std::shared_ptr<VesselClip>> vesselKeep;    // owns the clips (message thread)
    std::shared_ptr<const VesselClip> vesselUi;
    std::atomic<VesselClip*> vesselPending { nullptr };
    VesselClip* vesselCur = nullptr;                        // audio thread
    double vesselPhase = 0.0;
    float vesselGain = 0.0f;

    juce::TimeSliceThread srcThread { "Chloaelis source" };
    chl::LoopStream streams[2];
    float srcSelGain[2] { 1.0f, 0.0f }, srcLevelCur = 0.0f;

    // master feedback: each side's output back into its input, 16 samples later
    float fbRing[chl::numSides][64] {};
    int fbWrite = 0;
    float fbCur = 0.0f, fbDcX[chl::numSides] {}, fbDcY[chl::numSides] {};

    static constexpr int chunk = 16;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChloaelisProcessor)
};
