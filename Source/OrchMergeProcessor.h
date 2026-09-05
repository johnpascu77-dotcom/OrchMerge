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
//
// Sender role also carries an optional per-instance keyswitch exclusion
// (excludeKsNotes/ksZoneMin/ksZoneMax, 2026-09-05): a Sender commonly sits
// right after that track's own OrchNoteMapper, which has already remapped
// the ecosystem's shared keyswitch source zone into THIS instrument's own
// destination range - a merged reducer downstream (e.g. OrchPiano) that has
// no keyswitch concept at all otherwise treats those notes as real musical
// pitches. Off by default; the zone is per-instance because that destination
// range genuinely differs per instrument.
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

    // Diagnostics for the transport-stop stuck-note investigation
    // (2026-09-05): does the Hub's processBlock even keep running once the
    // host stops (some hosts suspend calls to a zero-tail-length effect once
    // there's no more input), and does pos->getIsPlaying() actually flip to
    // false when it does. Read from the message thread by the editor.
    int getHubBlockCountUi() const { return hubBlockCountUi.load(); }
    int getHubBlocksNotPlayingUi() const { return hubBlocksNotPlayingUi.load(); }
    int getHubStopEdgesUi() const { return hubStopEdgesUi.load(); }

private:
    // Sender role only: is `note` inside this instance's own keyswitch
    // exclusion zone (see createParameterLayout for why this lives per
    // Sender, not on the Hub or a shared default - found 2026-09-05: a real
    // rig's OrchNoteMapper remaps each instrument's keyswitch source notes
    // to its OWN destination range before the Sender taps the signal, so
    // the zone genuinely differs per instrument track).
    bool isNoteInKsExclusionZone (int note) const noexcept;
    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* roleParam = nullptr;
    std::atomic<float>* hubMarginMsParam = nullptr;
    std::atomic<float>* excludeKsNotesParam = nullptr;
    std::atomic<float>* ksZoneMinParam = nullptr;
    std::atomic<float>* ksZoneMaxParam = nullptr;

    double sampleRate = 44100.0;
    bool wasPlaying = false; // Hub role: detects the playing->stopped edge
    std::atomic<int> hubBlockCountUi { 0 };
    std::atomic<int> hubBlocksNotPlayingUi { 0 };
    std::atomic<int> hubStopEdgesUi { 0 };

    std::unique_ptr<OrchMergeLink> link;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchMergeAudioProcessor)
};
