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
    subtitleLabel.setText ("Hub: reports Hub Margin via Delay Compensation CC  |  compiled " ORCHMERGE_BUILD_TIMESTAMP,
                            juce::dontSendNotification);
    subtitleLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    subtitleLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (subtitleLabel);

    roleLabel.setText ("Role", juce::dontSendNotification);
    addAndMakeVisible (roleLabel);

    roleBox.addItemList ({ "Sender", "Hub" }, 1);
    addAndMakeVisible (roleBox);
    // Tooltips (text from Docs/OrchMerge_Guide.html): hovering a control or its label shows what it does.
    roleBox.setTooltip ("Defaults to Sender — safe to drop on a track before any Hub exists, since a Sender only ever tries to connect out, never binds anything. Exactly one instance in the whole rig should be Hub.");
    roleLabel.setTooltip ("Defaults to Sender — safe to drop on a track before any Hub exists, since a Sender only ever tries to connect out, never binds anything. Exactly one instance in the whole rig should be Hub.");
    marginSlider.setTooltip ("Hub role only: wall-clock slack held before placing an emitted event, absorbing ordinary Sender-to-Hub relay jitter so a genuine cross-instrument unison still lands close enough together for OrchPiano's own onset grouping to see it as one chord, not several.");
    marginLabel.setTooltip ("Hub role only: wall-clock slack held before placing an emitted event, absorbing ordinary Sender-to-Hub relay jitter so a genuine cross-instrument unison still lands close enough together for OrchPiano's own onset grouping to see it as one chord, not several.");
    excludeKsButton.setTooltip ("Sender role only. By the time a Sender taps a track, OrchNoteMapper has already remapped that instrument's keyswitches into its own destination zone — without this, a reducer downstream with no keyswitch concept treats them as real musical pitches. Off by default so existing rigs see no change.");
    ksMinSlider.setTooltip ("Bottom of the excluded zone — genuinely per-instance, since OrchNoteMapper's destination range differs per instrument (a flute lands around 24–33; bass instruments around 72–83).");
    ksMinLabel.setTooltip ("Bottom of the excluded zone — genuinely per-instance, since OrchNoteMapper's destination range differs per instrument (a flute lands around 24–33; bass instruments around 72–83).");
    ksMaxSlider.setTooltip ("Top of the excluded zone.");
    ksMaxLabel.setTooltip ("Top of the excluded zone.");
    // written by hand (controls the guide has no row for, labels, read-outs)
    delayCompensationCcSlider.setTooltip ("Hub: report the Hub margin as a delay in beats on this CC (default 113), so OrchCapture can shift the captured notes back by it. Sent at transport start, on any change and every 16 beats. 0 = off.");

    roleAttachment = std::make_unique<ComboBoxAttachment> (
        audioProcessor.getParameters(), "role", roleBox);

    marginLabel.setText ("Hub Margin (ms)", juce::dontSendNotification);
    addAndMakeVisible (marginLabel);

    marginSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    marginSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    addAndMakeVisible (marginSlider);
    marginAttachment = std::make_unique<SliderAttachment> (
        audioProcessor.getParameters(), "hubMarginMs", marginSlider);

    // Reports Hub Margin downstream (converted to FINE/sub-beat resolution,
    // see Docs §5d) so OrchCapture's own lookaheadCompensationFineCc (or
    // anything else that cares) can shift its capture back by the same
    // amount. Default CC 114 matches OrchCapture's own fine-CC default -
    // deliberately NOT the same CC/encoding as OrchPiano's own
    // delayCompensationCc (whole beats, CC 113), since Hub Margin is
    // sub-beat at any realistic tempo.
    delayCompensationCcLabel.setText ("Delay Comp. CC# (fine, 0=off)", juce::dontSendNotification);
    addAndMakeVisible (delayCompensationCcLabel);

    delayCompensationCcSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    delayCompensationCcSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    addAndMakeVisible (delayCompensationCcSlider);
    delayCompensationCcAttachment = std::make_unique<SliderAttachment> (
        audioProcessor.getParameters(), "delayCompensationCc", delayCompensationCcSlider);

    // Sender-role-only KS exclusion (2026-09-05, see OrchMergeProcessor's
    // createParameterLayout for the full story) - shown regardless of role
    // since the same instance toggles between them and the params should
    // stay visible/settable either way.
    excludeKsButton.setButtonText ("Exclude Keyswitch Notes");
    addAndMakeVisible (excludeKsButton);
    excludeKsAttachment = std::make_unique<ButtonAttachment> (
        audioProcessor.getParameters(), "excludeKsNotes", excludeKsButton);

    ksMinLabel.setText ("KS Zone Min", juce::dontSendNotification);
    addAndMakeVisible (ksMinLabel);
    ksMinSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    ksMinSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 20);
    addAndMakeVisible (ksMinSlider);
    ksMinAttachment = std::make_unique<SliderAttachment> (
        audioProcessor.getParameters(), "ksZoneMin", ksMinSlider);

    ksMaxLabel.setText ("KS Zone Max", juce::dontSendNotification);
    addAndMakeVisible (ksMaxLabel);
    ksMaxSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    ksMaxSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 20);
    addAndMakeVisible (ksMaxSlider);
    ksMaxAttachment = std::make_unique<SliderAttachment> (
        audioProcessor.getParameters(), "ksZoneMax", ksMaxSlider);

    statusLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (statusLabel);

    updateStatus();
    startTimerHz (4);

    setSize (440, 370);
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

            // Transport-stop stuck-note diagnostics (2026-09-05) - remove
            // once the live symptom is confirmed fixed or actually
            // root-caused differently.
            text << "\nHeld keys: " << link.getHeldKeyCountUi()
                 << "  |  stop-edges seen: " << audioProcessor.getHubStopEdgesUi()
                 << "  |  releases run: " << link.getReleaseAllHeldCallCountUi()
                 << "\nHub blocks: " << audioProcessor.getHubBlockCountUi()
                 << "  (not playing: " << audioProcessor.getHubBlocksNotPlayingUi() << ")";
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

    area.removeFromTop (6);
    auto delayCompensationCcRow = area.removeFromTop (26);
    delayCompensationCcLabel.setBounds (delayCompensationCcRow.removeFromLeft (140));
    delayCompensationCcSlider.setBounds (delayCompensationCcRow);

    area.removeFromTop (12);
    excludeKsButton.setBounds (area.removeFromTop (24));

    area.removeFromTop (6);
    auto ksMinRow = area.removeFromTop (26);
    ksMinLabel.setBounds (ksMinRow.removeFromLeft (110));
    ksMinSlider.setBounds (ksMinRow);

    area.removeFromTop (6);
    auto ksMaxRow = area.removeFromTop (26);
    ksMaxLabel.setBounds (ksMaxRow.removeFromLeft (110));
    ksMaxSlider.setBounds (ksMaxRow);

    area.removeFromTop (12);
    statusLabel.setBounds (area);
}
