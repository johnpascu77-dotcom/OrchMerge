#pragma once

#include <memory>
#include <JuceHeader.h>

class OrchMergeLink;

// OrchMerge - Phase 1: plugin skeleton, Sender pass-through only.
//
// A transparent MIDI-effect plugin, dropped on every source track (Sender
// role, default) plus one Hub track (Hub role - not yet implemented, see
// Docs/OrchMerge_Design.md). Whatever role is selected, this plugin never
// alters the MIDI passing through its own track - that invariant holds from
// Phase 1 onward, same as OrchCapture's own "observe/relay only" promise.
//
// Phase 1 exists to prove: it loads, the role param + discovery skeleton work,
// and dropping it onto a track before the Hub exists is completely safe.
// Note merging itself (the Hub side) is Phase 2+.
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

    OrchMergeLink& getLink() { return *link; }

private:
    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* roleParam = nullptr;

    std::unique_ptr<OrchMergeLink> link;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchMergeAudioProcessor)
};
