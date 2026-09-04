#include "OrchMergeEditor.h"
#include "OrchMergeBuildTimestamp.h"

OrchMergeAudioProcessorEditor::OrchMergeAudioProcessorEditor (OrchMergeAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    titleLabel.setText ("OrchMerge", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    addAndMakeVisible (titleLabel);

    // ORCHMERGE_BUILD_TIMESTAMP comes from a header CMake regenerates on
    // EVERY build (see GenerateBuildTimestamp.cmake) - unlike __DATE__/
    // __TIME__ baked into this one .cpp, it stays accurate even when an
    // incremental build only recompiled a different file for its own fix.
    subtitleLabel.setText ("Phase 3 - Hub ppq-aware emit buffer  |  compiled " ORCHMERGE_BUILD_TIMESTAMP,
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

    marginLabel.setText ("Hub Margin (ms)", juce::dontSendNotification);
    addAndMakeVisible (marginLabel);

    marginSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    marginSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    addAndMakeVisible (marginSlider);
    marginAttachment = std::make_unique<SliderAttachment> (
        audioProcessor.getParameters(), "hubMarginMs", marginSlider);

    statusLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (statusLabel);

    updateStatus();
    startTimerHz (4);

    setSize (440, 230);
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

    area.removeFromTop (8);

    auto marginRow = area.removeFromTop (26);
    marginLabel.setBounds (marginRow.removeFromLeft (110));
    marginSlider.setBounds (marginRow);

    area.removeFromTop (12);
    statusLabel.setBounds (area);
}
