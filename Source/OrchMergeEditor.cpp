#include "OrchMergeEditor.h"

OrchMergeAudioProcessorEditor::OrchMergeAudioProcessorEditor (OrchMergeAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    titleLabel.setText ("OrchMerge", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    addAndMakeVisible (titleLabel);

    subtitleLabel.setText ("Phase 1 - Sender skeleton", juce::dontSendNotification);
    subtitleLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    subtitleLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (subtitleLabel);

    roleLabel.setText ("Role", juce::dontSendNotification);
    addAndMakeVisible (roleLabel);

    roleBox.addItemList ({ "Sender", "Hub" }, 1);
    addAndMakeVisible (roleBox);
    roleAttachment = std::make_unique<ComboBoxAttachment> (
        audioProcessor.getParameters(), "role", roleBox);

    statusLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (statusLabel);

    updateStatus();
    startTimerHz (4);

    setSize (360, 200);
}

OrchMergeAudioProcessorEditor::~OrchMergeAudioProcessorEditor()
{
    stopTimer();
}

void OrchMergeAudioProcessorEditor::timerCallback()
{
    updateStatus();
}

void OrchMergeAudioProcessorEditor::updateStatus()
{
    const auto mode = audioProcessor.getLink().getMode();

    juce::String text;
    if (mode == OrchMergeLink::Mode::HubNotBuilt)
    {
        text = "Hub role selected - not yet implemented (Phase 2).\n"
               "This track's MIDI still passes through unchanged.";
    }
    else
    {
        text = audioProcessor.getLink().isSenderConnected()
                   ? "Connected to Hub."
                   : "Waiting for Hub (port " + juce::String (OrchMergeLink::kPort) + ")...";
    }

    statusLabel.setText (text, juce::dontSendNotification);
}

void OrchMergeAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void OrchMergeAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);

    titleLabel.setBounds (area.removeFromTop (28));
    subtitleLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (12);

    auto roleRow = area.removeFromTop (26);
    roleLabel.setBounds (roleRow.removeFromLeft (50));
    roleBox.setBounds (roleRow);

    area.removeFromTop (12);
    statusLabel.setBounds (area);
}
