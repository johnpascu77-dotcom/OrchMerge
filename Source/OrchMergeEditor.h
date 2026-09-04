#pragma once

#include <JuceHeader.h>
#include "OrchMergeProcessor.h"
#include "OrchMergeLink.h"

class OrchMergeAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       private juce::Timer
{
public:
    explicit OrchMergeAudioProcessorEditor (OrchMergeAudioProcessor&);
    ~OrchMergeAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateStatus();

    OrchMergeAudioProcessor& audioProcessor;

    juce::Label titleLabel, subtitleLabel;
    juce::Label roleLabel;
    juce::ComboBox roleBox;
    juce::Label statusLabel;

    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<ComboBoxAttachment> roleAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchMergeAudioProcessorEditor)
};
