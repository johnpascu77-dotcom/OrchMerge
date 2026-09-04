#include "OrchMergeEditor.h"

OrchMergeAudioProcessorEditor::OrchMergeAudioProcessorEditor (OrchMergeAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    titleLabel.setText ("OrchMerge", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    addAndMakeVisible (titleLabel);

    subtitleLabel.setText ("Phase 2 - Hub naive merge  |  compiled " __DATE__ " " __TIME__,
                            juce::dontSendNotification);
    subtitleLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
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

    setSize (440, 200);
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
    const auto& link = audioProcessor.getLink();

    juce::String text;
    switch (link.getMode())
    {
        case OrchMergeLink::Mode::Sender:
            text = link.isSenderConnected()
                       ? "Connected to Hub."
                       : "Waiting for Hub (port " + juce::String (OrchMergeLink::kPort) + ")...";
            break;

        case OrchMergeLink::Mode::Hub:
        {
            const int count = link.getConnectedSenderCountForUi();
            text = juce::String (count) + (count == 1 ? " sender connected." : " senders connected.");

            if (const auto lastMs = link.getLastRelayedNoteMsForUi(); lastMs > 0)
            {
                const double secondsAgo = (juce::Time::currentTimeMillis() - lastMs) / 1000.0;
                text << "\nLast note relayed " << juce::String (secondsAgo, 1) << "s ago.";
            }
            else
            {
                text << "\nNo notes relayed yet.";
            }
            break;
        }

        case OrchMergeLink::Mode::HubPortBusy:
            text = "Port " + juce::String (OrchMergeLink::kPort) + " is already in use - "
                   "another Hub instance may already be bound to it.";
            break;
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
