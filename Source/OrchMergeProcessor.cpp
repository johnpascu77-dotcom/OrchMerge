#include "OrchMergeProcessor.h"
#include "OrchMergeEditor.h"
#include "OrchMergeLink.h"

#include <utility>

namespace
{
    // Must match OrchCaptureProcessor.cpp's own kFineBeatScale exactly - see
    // that constant's own doc comment for the full reasoning (0-127 covers
    // 0-1.98 beats at ~0.0156-beat resolution, comfortably covering Hub
    // Margin's 10-500ms range up to ~240bpm before clipping).
    constexpr double kFineBeatScale = 64.0;
}

OrchMergeAudioProcessor::OrchMergeAudioProcessor()
    : AudioProcessor (BusesProperties()),
      parameters (*this, nullptr, "OrchMergeParameters", createParameterLayout())
{
    roleParam = parameters.getRawParameterValue ("role");
    hubMarginMsParam = parameters.getRawParameterValue ("hubMarginMs");
    excludeKsNotesParam = parameters.getRawParameterValue ("excludeKsNotes");
    ksZoneMinParam = parameters.getRawParameterValue ("ksZoneMin");
    ksZoneMaxParam = parameters.getRawParameterValue ("ksZoneMax");
    delayCompensationCcParam = parameters.getRawParameterValue ("delayCompensationCc");

    link = std::make_unique<OrchMergeLink> (*this);
}

OrchMergeAudioProcessor::~OrchMergeAudioProcessor()
{
    link.reset();
}

juce::AudioProcessorValueTreeState::ParameterLayout OrchMergeAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Sender (default): safe to drop onto a track before the Hub exists - it
    // only ever tries to connect out, never binds a port. Hub: not implemented
    // in Phase 1 (see OrchMergeLink); reserved so instances can be switched
    // over once Phase 2 lands without re-authoring the template.
    params.push_back (std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { "role", 1 }, "Role",
        juce::StringArray { "Sender", "Hub" }, 0));

    // Hub role only: wall-clock slack held before placing a decided-to-emit
    // event, absorbing Sender-to-Hub relay jitter so genuinely-simultaneous
    // cross-instrument unisons land close enough together for a downstream
    // reducer's own onset grouping to still see them as one onset. Default
    // 60ms - see Docs/OrchMerge_Design.md SS3.
    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "hubMarginMs", 1 }, "Hub Margin (ms)", 10, 500, 60));

    // Sender role only: found 2026-09-05 - a real rig's OrchNoteMapper remaps
    // each instrument's source keyswitch notes (e.g. the ecosystem's own
    // unified 12-23 zone) to that instrument's OWN destination range before
    // this Sender taps the track's signal, so a merged reducer downstream
    // (OrchPiano) that has no keyswitch concept at all treats them as real
    // musical pitches - confirmed live: a 24-note cluster of near-duplicate
    // pitches 24-30 landed on a piano reduction channel at take-start, one
    // per instrument's initial articulation-select firing together. Off by
    // default (existing rigs without this problem see no behavior change);
    // zone is per-instance because OrchNoteMapper's destination range
    // genuinely differs per instrument (seen: flute 24-33; this ecosystem's
    // own OrchCapture notes elsewhere that bass instruments land in 72-83).
    params.push_back (std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "excludeKsNotes", 1 }, "Exclude Keyswitch Notes", false));
    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "ksZoneMin", 1 }, "KS Zone Min", 0, 127, 24));
    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "ksZoneMax", 1 }, "KS Zone Max", 0, 127, 35));

    // Hub role only: reports Hub Margin downstream, in FINE (sub-beat)
    // resolution (converted from hubMarginMs using the current tempo), so
    // OrchCapture (or anything else that cares) can shift its own capture
    // back by the same amount instead of landing hubMarginMs late - see
    // Docs/OrchMerge_Design.md §5d. Deliberately NOT the same CC/encoding as
    // OrchPiano's own delayCompensationCc (whole beats, CC 113): Hub Margin
    // (10-500ms) is sub-beat at any realistic tempo, and a whole-beats-only
    // encoding rounds anything under 0.5 beats down to 0, silently
    // discarding it entirely (found live 2026-09-16 - the very first attempt
    // at this feature used CC 113/whole-beats and produced zero effect).
    // Targets OrchCapture's own lookaheadCompensationFineCc instead, default
    // 114, matching (see OrchCaptureProcessor's own kFineBeatScale for the
    // exact wire encoding both sides must agree on). 0 = off.
    params.push_back (std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "delayCompensationCc", 1 }, "Delay Compensation CC# (0=off)", 0, 127, 114));

    return { params.begin(), params.end() };
}

void OrchMergeAudioProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
}

void OrchMergeAudioProcessor::releaseResources() {}

bool OrchMergeAudioProcessor::isBusesLayoutSupported (const BusesLayout&) const
{
    return true;
}

void OrchMergeAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    buffer.clear();

    if (isHubRoleParamOn())
    {
        // Hub: this track carries no source material of its own - whatever
        // arrived on it is discarded, and it emits only what the Link has
        // decided (from every connected Sender) is ready to place in this
        // block, at the sample position its own ppq maps to.
        midiMessages.clear();

        hubBlockCountUi.fetch_add (1);

        double blockStartPpq = 0.0;
        double ppqPerSample = 0.0;
        double bpm = 120.0;
        bool playing = false;

        if (auto* transport = getPlayHead())
        {
            if (const auto pos = transport->getPosition())
            {
                if (const auto ppqOpt = pos->getPpqPosition())
                {
                    blockStartPpq = *ppqOpt;

                    if (const auto hostBpm = pos->getBpm(); hostBpm && *hostBpm > 0.0)
                        bpm = *hostBpm;

                    ppqPerSample = sampleRate > 0.0 ? (bpm / 60.0) / sampleRate : 0.0;
                    link->setCurrentBpm (bpm);
                }

                playing = pos->getIsPlaying();
                if (! playing)
                    hubBlocksNotPlayingUi.fetch_add (1);
                if (wasPlaying && ! playing)
                {
                    hubStopEdgesUi.fetch_add (1);
                    link->releaseAllHeld();
                }
                wasPlaying = playing;
            }
        }

        for (const auto& ev : link->drainEmitEvents (blockStartPpq, ppqPerSample, buffer.getNumSamples()))
        {
            const auto message = ev.isNoteOn
                ? juce::MidiMessage::noteOn (ev.channel, ev.note, static_cast<juce::uint8> (ev.velocity))
                : juce::MidiMessage::noteOff (ev.channel, ev.note, static_cast<juce::uint8> (ev.velocity));
            midiMessages.addEvent (message, ev.sampleOffset);
        }

        // Reports Hub Margin downstream so OrchCapture (or anything else
        // that cares) can shift its own capture back by the same amount
        // instead of landing hubMarginMs late - see Docs §5d. Same pattern
        // as OrchPiano's own delayCompensationCc: sent at transport start,
        // on any value change, and re-sent every 16 beats as a safety net
        // for a late-loading listener. A plain CC, observed and passed
        // through - never consumed - by anything downstream.
        if (playing)
        {
            const int compCc = delayCompensationCcParam != nullptr
                ? juce::roundToInt (delayCompensationCcParam->load()) : 0;
            if (compCc > 0)
            {
                const int hubMarginMs = getHubMarginMs();
                const double delayBeats = static_cast<double> (hubMarginMs) / 1000.0 * (bpm / 60.0);
                const int ccValue = juce::jlimit (0, 127, juce::roundToInt (delayBeats * kFineBeatScale));

                const bool valueChanged = ccValue != lastSentDelayCcValue;
                const bool dueForResend = blockStartPpq - lastDelayCcSentPpq >= 16.0;
                if (valueChanged || dueForResend)
                {
                    midiMessages.addEvent (juce::MidiMessage::controllerEvent (1, compCc, ccValue), 0);
                    lastSentDelayCcValue = ccValue;
                    lastDelayCcSentPpq = blockStartPpq;
                }
            }
        }
        else
        {
            lastSentDelayCcValue = -1;
            lastDelayCcSentPpq = -1.0e18;
        }

        return;
    }

    // Sender: transparent pass-through - midiMessages is left exactly as the
    // host handed it to us. We only *observe* note on/off to forward to the
    // Hub; nothing here alters what downstream on this track receives.
    auto* transport = getPlayHead();
    if (transport == nullptr)
        return;

    const auto pos = transport->getPosition();
    if (! pos || ! pos->getIsPlaying())
        return;

    const auto ppqOpt = pos->getPpqPosition();
    if (! ppqOpt)
        return;

    double bpm = 120.0;
    if (const auto hostBpm = pos->getBpm(); hostBpm && *hostBpm > 0.0)
        bpm = *hostBpm;

    const double blockStartPpq = *ppqOpt;
    const double ppqPerSample = sampleRate > 0.0 ? (bpm / 60.0) / sampleRate : 0.0;

    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();
        if (! message.isNoteOnOrOff())
            continue;

        const int note = juce::jlimit (0, 127, message.getNoteNumber());
        if (isNoteInKsExclusionZone (note))
            continue; // never observed by the Hub - the real instrument downstream
                       // of this Sender still receives it untouched either way.

        OrchMergeLink::NoteEvent ev;
        ev.isNoteOn = message.isNoteOn() && message.getVelocity() > 0;
        ev.channel = juce::jlimit (1, 16, message.getChannel());
        ev.note = note;
        ev.velocity = juce::jlimit (0, 127, static_cast<int> (message.getVelocity()));
        ev.ppq = blockStartPpq + metadata.samplePosition * ppqPerSample;

        link->pushOutgoingEvent (ev);
    }
}

bool OrchMergeAudioProcessor::isHubRoleParamOn() const
{
    return roleParam != nullptr && roleParam->load() > 0.5f;
}

bool OrchMergeAudioProcessor::isNoteInKsExclusionZone (int note) const noexcept
{
    if (excludeKsNotesParam == nullptr || excludeKsNotesParam->load() < 0.5f)
        return false;

    int lo = ksZoneMinParam != nullptr ? juce::roundToInt (ksZoneMinParam->load()) : 24;
    int hi = ksZoneMaxParam != nullptr ? juce::roundToInt (ksZoneMaxParam->load()) : 35;
    if (lo > hi)
        std::swap (lo, hi);
    return note >= lo && note <= hi;
}

int OrchMergeAudioProcessor::getHubMarginMs() const
{
    return hubMarginMsParam != nullptr ? juce::roundToInt (hubMarginMsParam->load()) : 60;
}

juce::AudioProcessorEditor* OrchMergeAudioProcessor::createEditor()
{
    return new OrchMergeAudioProcessorEditor (*this);
}

bool OrchMergeAudioProcessor::hasEditor() const { return true; }
const juce::String OrchMergeAudioProcessor::getName() const { return JucePlugin_Name; }
bool OrchMergeAudioProcessor::acceptsMidi() const { return true; }
bool OrchMergeAudioProcessor::producesMidi() const { return true; }
bool OrchMergeAudioProcessor::isMidiEffect() const { return true; }
double OrchMergeAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int OrchMergeAudioProcessor::getNumPrograms() { return 1; }
int OrchMergeAudioProcessor::getCurrentProgram() { return 0; }
void OrchMergeAudioProcessor::setCurrentProgram (int) {}
const juce::String OrchMergeAudioProcessor::getProgramName (int) { return {}; }
void OrchMergeAudioProcessor::changeProgramName (int, const juce::String&) {}

void OrchMergeAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto state = parameters.copyState(); state.isValid())
    {
        if (auto xml = state.createXml())
            copyXmlToBinary (*xml, destData);
    }
}

void OrchMergeAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OrchMergeAudioProcessor();
}
