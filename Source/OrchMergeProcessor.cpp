#include "OrchMergeProcessor.h"
#include "OrchMergeEditor.h"
#include "OrchMergeLink.h"

OrchMergeAudioProcessor::OrchMergeAudioProcessor()
    : AudioProcessor (BusesProperties()),
      parameters (*this, nullptr, "OrchMergeParameters", createParameterLayout())
{
    roleParam = parameters.getRawParameterValue ("role");

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
        // decided (from every connected Sender) is ready to relay right now.
        midiMessages.clear();

        for (const auto& ev : link->drainEmitEvents())
        {
            const auto message = ev.isNoteOn
                ? juce::MidiMessage::noteOn (ev.channel, ev.note, static_cast<juce::uint8> (ev.velocity))
                : juce::MidiMessage::noteOff (ev.channel, ev.note, static_cast<juce::uint8> (ev.velocity));
            midiMessages.addEvent (message, 0);
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

        OrchMergeLink::NoteEvent ev;
        ev.isNoteOn = message.isNoteOn() && message.getVelocity() > 0;
        ev.channel = juce::jlimit (1, 16, message.getChannel());
        ev.note = juce::jlimit (0, 127, message.getNoteNumber());
        ev.velocity = juce::jlimit (0, 127, static_cast<int> (message.getVelocity()));
        ev.ppq = blockStartPpq + metadata.samplePosition * ppqPerSample;

        link->pushOutgoingEvent (ev);
    }
}

bool OrchMergeAudioProcessor::isHubRoleParamOn() const
{
    return roleParam != nullptr && roleParam->load() > 0.5f;
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
