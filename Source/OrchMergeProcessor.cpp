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

void OrchMergeAudioProcessor::prepareToPlay (double, int) {}
void OrchMergeAudioProcessor::releaseResources() {}

bool OrchMergeAudioProcessor::isBusesLayoutSupported (const BusesLayout&) const
{
    return true;
}

void OrchMergeAudioProcessor::processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    // Transparent pass-through: the incoming MidiBuffer is left exactly as the
    // host handed it to us, in every role. Note observation/merging is Phase 2+.
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
