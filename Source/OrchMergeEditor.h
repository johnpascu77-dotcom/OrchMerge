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
    juce::Label marginLabel;
    juce::Slider marginSlider;
    juce::Label statusLabel;

    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<ComboBoxAttachment> roleAttachment;
    std::unique_ptr<SliderAttachment> marginAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchMergeAudioProcessorEditor)
};
