#include "OrchMergeLink.h"
#include "OrchMergeProcessor.h"

namespace
{
    constexpr int kPollMs = 3000;           // worker loop interval
    constexpr int kConnectTimeoutMs = 300;  // blocking connect - worker thread only, never the message thread
}

// callbacksOnMessageThread == false: connection callbacks arrive on the
// connection's own background thread, never the host message thread.
class OrchMergeLink::SenderConnection : public juce::InterprocessConnection
{
public:
    explicit SenderConnection (OrchMergeLink& o) : juce::InterprocessConnection (false), owner (o) {}
    ~SenderConnection() override { disconnect(); }

    void connectionMade() override { owner.senderConnected.store (true); }
    void connectionLost() override { owner.senderConnected.store (false); }
    void messageReceived (const juce::MemoryBlock&) override {} // no wire format yet (Phase 2)

private:
    OrchMergeLink& owner;
};

OrchMergeLink::OrchMergeLink (OrchMergeAudioProcessor& p)
    : juce::Thread ("OrchMerge link"), processor (p)
{
    startThread (juce::Thread::Priority::background);
}

OrchMergeLink::~OrchMergeLink()
{
    signalThreadShouldExit();
    notify();
    stopThread (3000);

    sender.reset();
}

juce::File OrchMergeLink::lockFile()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("orchmerge-hub.lock");
}

void OrchMergeLink::run()
{
    while (! threadShouldExit())
    {
        reconcileMode();

        if (mode.load() == Mode::Sender)
            serviceSender();

        wait (kPollMs);
    }

    sender.reset();
}

void OrchMergeLink::reconcileMode()
{
    const bool wantHub = processor.isHubRoleParamOn();

    if (wantHub)
    {
        // Phase 1: the Hub role exists as a selectable value but is not
        // implemented - no port bind, no lock file. See Phase 2.
        if (sender != nullptr)
        {
            sender.reset();
            senderConnected.store (false);
        }

        mode.store (Mode::HubNotBuilt);
    }
    else
    {
        mode.store (Mode::Sender);

        if (sender == nullptr)
            sender = std::make_unique<SenderConnection> (*this);
    }
}

void OrchMergeLink::serviceSender()
{
    if (sender == nullptr)
        return;

    if (! sender->isConnected())
    {
        senderConnected.store (false);

        // The gate: only reach for the socket once a Hub has announced itself.
        // No Hub in the rig == one cheap file check per poll.
        if (lockFile().existsAsFile())
            sender->connectToSocket ("127.0.0.1", kPort, kConnectTimeoutMs);
    }

    // No payload push yet - the Sender->Hub wire format is decided in Phase 2.
}
