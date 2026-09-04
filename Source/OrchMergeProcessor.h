#pragma once

#include <memory>
#include <JuceHeader.h>

class OrchMergeLink;

// OrchMerge - Phase 3: Hub ppq-aware emit buffer.
//
// Dropped on every source track (Sender role, default) plus one Hub track
// (Hub role). A Sender never alters the MIDI passing through its own track -
// it only *observes* note on/off events (tagged with its own ppq) to forward
// to the Hub via OrchMergeLink. The Hub track's own incoming MIDI is not
// relevant - its processBlock instead emits whatever the Link has merged from
// every connected Sender, each placed at the sample position its own ppq
// maps to once hubMarginMs of relay jitter has been absorbed (see
// OrchMergeLink and Docs/OrchMerge_Design.md SS3).
class OrchMergeAudioProcessor final : public juce::AudioProcessor
{
public:
    OrchMergeAudioProcessor();
    ~OrchMergeAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // ---- Message-thread API for the editor / Link -------------------------

    // true when the `role` param is set to Hub.
    bool isHubRoleParamOn() const;

    // Hub role only: how long (ms of wall-clock time) the Hub holds a
    // decided-to-emit event before placing it, absorbing Sender-to-Hub relay
    // jitter. See OrchMergeLink.
    int getHubMarginMs() const;

    OrchMergeLink& getLink() { return *link; }

private:
    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* roleParam = nullptr;
    std::atomic<float>* hubMarginMsParam = nullptr;

    double sampleRate = 44100.0;

    std::unique_ptr<OrchMergeLink> link;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchMergeAudioProcessor)
};
