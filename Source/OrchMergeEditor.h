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
    juce::Label delayCompensationCcLabel;
    juce::Slider delayCompensationCcSlider;
    juce::ToggleButton excludeKsButton;
    juce::Label ksMinLabel, ksMaxLabel;
    juce::Slider ksMinSlider, ksMaxSlider;
    juce::Label statusLabel;

    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<ComboBoxAttachment> roleAttachment;
    std::unique_ptr<SliderAttachment> marginAttachment;
    std::unique_ptr<SliderAttachment> delayCompensationCcAttachment;
    std::unique_ptr<ButtonAttachment> excludeKsAttachment;
    std::unique_ptr<SliderAttachment> ksMinAttachment, ksMaxAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchMergeAudioProcessorEditor)
};
