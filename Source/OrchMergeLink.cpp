#include "OrchMergeLink.h"
#include "OrchMergeProcessor.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr int kIdlePollMs = 1000;    // no Hub / no work: cheap discovery poll
    constexpr int kActivePollMs = 5;     // Sender connected: drain the outgoing queue promptly
    constexpr int kConnectTimeoutMs = 300; // blocking connect - worker thread only, never the message thread
    constexpr size_t kMaxQueuedOutgoing = 4096; // safety rail if the Hub side stalls

    // Sentinel ppq for a force-released note-off (a dropped Sender connection):
    // there is no "correct" musical position for an out-of-band release, so
    // drainEmitEvents should always treat it as already due, regardless of
    // whatever block is currently playing.
    constexpr double kImmediatePpq = -1.0e9;

    // Fixed 28-byte wire message: type(1) + senderUid(16) + channel(1) +
    // note(1) + velocity(1) + ppq(8, double). Deliberately not JSON (unlike
    // OrchCaptureLink's once-per-take blob) - this runs continuously for the
    // life of the transport, so per-message overhead matters.
    constexpr int kMessageSize = 1 + 16 + 1 + 1 + 1 + 8;

    juce::MemoryBlock encodeEvent (const juce::Uuid& uid, const OrchMergeLink::NoteEvent& ev)
    {
        juce::MemoryBlock block;
        juce::MemoryOutputStream out (block, false);
        out.writeByte (static_cast<char> (ev.isNoteOn ? 1 : 0));
        out.write (uid.getRawData(), 16);
        out.writeByte (static_cast<char> (juce::jlimit (0, 127, ev.channel)));
        out.writeByte (static_cast<char> (juce::jlimit (0, 127, ev.note)));
        out.writeByte (static_cast<char> (juce::jlimit (0, 127, ev.velocity)));
        out.writeDouble (ev.ppq);
        out.flush();
        return block;
    }

    struct DecodedEvent
    {
        bool ok = false;
        std::string senderKey; // raw 16-byte uid, used as a map key
        bool isNoteOn = true;
        int channel = 1, note = 60, velocity = 100;
        double ppq = 0.0;
    };

    DecodedEvent decodeEvent (const juce::MemoryBlock& message)
    {
        DecodedEvent out;
        if (message.getSize() != static_cast<size_t> (kMessageSize))
            return out;

        juce::MemoryInputStream in (message, false);
        out.isNoteOn = in.readByte() != 0;

        char rawUid[16];
        if (in.read (rawUid, 16) != 16)
            return out;
        out.senderKey.assign (rawUid, 16);

        out.channel = static_cast<juce::uint8> (in.readByte());
        out.note = static_cast<juce::uint8> (in.readByte());
        out.velocity = static_cast<juce::uint8> (in.readByte());
        out.ppq = in.readDouble();
        out.ok = true;
        return out;
    }
}

// callbacksOnMessageThread == false: every connection callback arrives on the
// connection's own background thread, never the host message thread.

class OrchMergeLink::SenderConnection : public juce::InterprocessConnection
{
public:
    explicit SenderConnection (OrchMergeLink& o) : juce::InterprocessConnection (false), owner (o) {}
    ~SenderConnection() override { disconnect(); }

    void connectionMade() override { owner.senderConnected.store (true); }
    void connectionLost() override { owner.senderConnected.store (false); }
    void messageReceived (const juce::MemoryBlock&) override {} // the Hub never replies

private:
    OrchMergeLink& owner;
};

class OrchMergeLink::HubConnection : public juce::InterprocessConnection
{
public:
    explicit HubConnection (OrchMergeLink& ownerIn) : juce::InterprocessConnection (false), owner (ownerIn) {}
    ~HubConnection() override { disconnect(); }

    void connectionMade() override {}
    void connectionLost() override { owner.onHubConnectionLost (this); }

    void messageReceived (const juce::MemoryBlock& message) override
    {
        owner.onHubMessage (this, message);
    }

private:
    OrchMergeLink& owner;
};

class OrchMergeLink::HubServer : public juce::InterprocessConnectionServer
{
public:
    explicit HubServer (OrchMergeLink& ownerIn) : owner (ownerIn) {}
    ~HubServer() override { stop(); }

    juce::InterprocessConnection* createConnectionObject() override
    {
        auto connection = std::make_unique<HubConnection> (owner);
        auto* raw = connection.get();
        owner.registerHubConnection (std::move (connection));
        return raw;
    }

private:
    OrchMergeLink& owner;
};

// ============================== link ==================================

OrchMergeLink::OrchMergeLink (OrchMergeAudioProcessor& p)
    : juce::Thread ("OrchMerge link"), processor (p), localUid()
{
    outgoingEvents.reserve (256);
    startThread (juce::Thread::Priority::background);
}

OrchMergeLink::~OrchMergeLink()
{
    signalThreadShouldExit();
    notify();
    stopThread (3000);

    sender.reset();
    teardownHub();
}

juce::File OrchMergeLink::lockFile()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("orchmerge-hub.lock");
}

void OrchMergeLink::teardownHub()
{
    if (hubServer != nullptr)
    {
        hubServer->stop();
        hubServer.reset();
    }

    {
        std::lock_guard<std::mutex> lock (hubMutex);
        hubConnections.clear();
        connectionToSender.clear();
        holdCounts.clear();
        bySender.clear();
        pendingEmits.clear();
    }
    connectedSenderCount.store (0);

    if (wroteLockFile)
    {
        lockFile().deleteFile();
        wroteLockFile = false;
    }
}

// ---- worker thread ----

void OrchMergeLink::run()
{
    while (! threadShouldExit())
    {
        reconcileMode();

        int waitMs = kIdlePollMs;

        if (mode.load() == Mode::Sender)
        {
            serviceSender();
            waitMs = (sender != nullptr && sender->isConnected()) ? kActivePollMs : kIdlePollMs;
        }

        wait (waitMs);
    }

    sender.reset();
    teardownHub();
}

void OrchMergeLink::reconcileMode()
{
    const bool wantHub = processor.isHubRoleParamOn();

    if (wantHub)
    {
        if (sender != nullptr)
        {
            sender.reset();
            senderConnected.store (false);
        }

        if (hubServer == nullptr)
        {
            auto candidate = std::make_unique<HubServer> (*this);
            if (candidate->beginWaitingForSocket (kPort))
            {
                hubServer = std::move (candidate);
                mode.store (Mode::Hub);
            }
            else
            {
                mode.store (Mode::HubPortBusy);
            }
        }
        else
        {
            mode.store (Mode::Hub);
        }

        if (hubServer != nullptr && ! wroteLockFile)
        {
            lockFile().replaceWithText (juce::String (kPort));
            wroteLockFile = true;
        }
    }
    else
    {
        if (hubServer != nullptr || wroteLockFile)
            teardownHub();

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

        // The gate: only reach for the socket once a Hub has announced
        // itself. No Hub in the rig == one cheap file check per poll.
        if (lockFile().existsAsFile())
            sender->connectToSocket ("127.0.0.1", kPort, kConnectTimeoutMs);

        return;
    }

    std::vector<NoteEvent> batch;
    {
        const juce::SpinLock::ScopedLockType lock (outgoingLock);
        batch.swap (outgoingEvents);
    }

    for (const auto& ev : batch)
        sender->sendMessage (encodeEvent (localUid, ev));
}

void OrchMergeLink::pushOutgoingEvent (const NoteEvent& ev)
{
    const juce::SpinLock::ScopedLockType lock (outgoingLock);
    if (outgoingEvents.size() < kMaxQueuedOutgoing)
        outgoingEvents.push_back (ev);
}

// ---- Hub side (connection threads) ----

void OrchMergeLink::registerHubConnection (std::unique_ptr<HubConnection> connection)
{
    std::lock_guard<std::mutex> lock (hubMutex);
    hubConnections.push_back (std::move (connection));
}

void OrchMergeLink::onHubMessage (HubConnection* connection, const juce::MemoryBlock& message)
{
    const auto decoded = decodeEvent (message);
    if (! decoded.ok)
        return;

    std::lock_guard<std::mutex> lock (hubMutex);

    if (connectionToSender.find (connection) == connectionToSender.end())
    {
        connectionToSender[connection] = decoded.senderKey;
        connectedSenderCount.store (static_cast<int> (connectionToSender.size()));
    }

    const auto key = std::make_pair (decoded.channel, decoded.note);

    // Convert the wall-clock margin param into beats at the tempo the Hub's
    // own transport reports right now (setCurrentBpm, called each block from
    // processBlock) so readiness is gated on the same shared ppq timeline
    // drainEmitEvents places events on - not on whenever this message
    // happened to arrive on its own connection thread.
    const auto marginMs = static_cast<double> (processor.getHubMarginMs());
    const auto marginBeats = (marginMs / 1000.0) * (currentBpm.load() / 60.0);
    const auto readyAtPpq = decoded.ppq + marginBeats;

    if (decoded.isNoteOn)
    {
        ++bySender[decoded.senderKey][key];

        auto& count = holdCounts[key];
        const bool isFirstHolder = (count == 0);
        ++count;

        if (isFirstHolder)
            pendingEmits.push_back ({ readyAtPpq, decoded.ppq,
                                       { true, decoded.channel, decoded.note, decoded.velocity, 0 } });
        // else: this pitch is already sounding downstream from another
        // holder (a real orchestration doubling, or divisi within one
        // sender's own track) - just add to the count. Previously every
        // NoteOn was relayed unconditionally, so any unison produced a
        // second/third stacked notehead instead of merging into the one
        // already-emitted note.
    }
    else
    {
        auto senderIt = bySender.find (decoded.senderKey);
        if (senderIt != bySender.end())
        {
            auto keyIt = senderIt->second.find (key);
            if (keyIt != senderIt->second.end())
            {
                if (--keyIt->second <= 0)
                    senderIt->second.erase (keyIt);

                auto countIt = holdCounts.find (key);
                if (countIt != holdCounts.end())
                {
                    if (--countIt->second <= 0)
                    {
                        holdCounts.erase (countIt);
                        pendingEmits.push_back ({ readyAtPpq, decoded.ppq,
                                                   { false, decoded.channel, decoded.note, decoded.velocity, 0 } });
                    }
                    // else: another holder still sounds this pitch -
                    // suppress the note-off so it doesn't kill their
                    // still-sounding note (SS4 of the design doc).
                }
            }

            if (senderIt->second.empty())
                bySender.erase (senderIt);
        }
    }

    lastRelayedNoteMs.store (juce::Time::currentTimeMillis());
}

void OrchMergeLink::onHubConnectionLost (HubConnection* connection)
{
    juce::WeakReference<OrchMergeLink> weak (this);

    juce::MessageManager::callAsync ([weak, connection]
    {
        auto* self = weak.get();
        if (self == nullptr)
            return;

        std::string senderKey;
        {
            std::lock_guard<std::mutex> lock (self->hubMutex);
            auto it = self->connectionToSender.find (connection);
            if (it != self->connectionToSender.end())
            {
                senderKey = it->second;
                self->connectionToSender.erase (it);
            }
            self->connectedSenderCount.store (static_cast<int> (self->connectionToSender.size()));

            self->hubConnections.erase (
                std::remove_if (self->hubConnections.begin(), self->hubConnections.end(),
                                 [connection] (const std::unique_ptr<HubConnection>& e)
                                 { return e.get() == connection; }),
                self->hubConnections.end());
        }

        // A crashed/removed Sender never sends its trailing note-offs -
        // force-release everything it was still holding so the Hub doesn't
        // leave a stuck note sounding downstream.
        if (! senderKey.empty())
            self->releaseAllForSender (senderKey);
    });
}

void OrchMergeLink::releaseAllForSender (const std::string& senderKey)
{
    std::lock_guard<std::mutex> lock (hubMutex);

    auto bySenderIt = bySender.find (senderKey);
    if (bySenderIt == bySender.end())
        return;

    for (const auto& [key, heldCount] : bySenderIt->second)
    {
        auto countIt = holdCounts.find (key);
        if (countIt == holdCounts.end())
            continue;

        countIt->second -= heldCount;
        if (countIt->second <= 0)
        {
            holdCounts.erase (countIt);
            pendingEmits.push_back ({ kImmediatePpq, kImmediatePpq,
                                       { false, key.first, key.second, 0, 0 } });
        }
    }

    bySender.erase (bySenderIt);
}

void OrchMergeLink::releaseAllHeld()
{
    std::lock_guard<std::mutex> lock (hubMutex);

    for (const auto& [key, count] : holdCounts)
    {
        if (count > 0)
            pendingEmits.push_back ({ kImmediatePpq, kImmediatePpq,
                                       { false, key.first, key.second, 0, 0 } });
    }
    holdCounts.clear();
    bySender.clear();
}

// ---- Hub side (audio thread) ----

std::vector<OrchMergeLink::EmitEvent> OrchMergeLink::drainEmitEvents (
    double blockStartPpq, double ppqPerSample, int numSamples)
{
    std::vector<EmitEvent> out;

    // try_lock, not lock: the audio thread must never block on IPC-side
    // contention. If a connection thread holds hubMutex right now, these
    // events simply wait for the next block - a few milliseconds, harmless
    // given hubMarginMs is already tens of ms.
    if (! hubMutex.try_lock())
        return out;

    // readyAtPpq is gated against this block's END, not its start: an event
    // becomes eligible the moment playback reaches the point where its own
    // margin has elapsed, which may be partway through the current block.
    const double blockEndPpq = blockStartPpq + static_cast<double> (numSamples) * ppqPerSample;

    std::vector<PendingEmit> stillPending;
    stillPending.reserve (pendingEmits.size());

    for (const auto& pending : pendingEmits)
    {
        if (ppqPerSample > 0.0 && pending.readyAtPpq > blockEndPpq)
        {
            stillPending.push_back (pending);
            continue;
        }

        // No separate "has pending.ppq itself arrived yet" check is needed
        // here: readyAtPpq = ppq + marginBeats with marginBeats >= 0, so
        // readyAtPpq <= blockEndPpq (just tested above) already implies
        // ppq <= blockEndPpq. A ppq that lands before blockStartPpq (the
        // event waited across more than one block) is simply clamped to
        // sample 0 below, same as it always was.
        int sampleOffset = 0;

        if (ppqPerSample > 0.0 && pending.ppq > kImmediatePpq)
        {
            const double samplesFromBlockStart = (pending.ppq - blockStartPpq) / ppqPerSample;
            sampleOffset = juce::jlimit (0, juce::jmax (0, numSamples - 1),
                                          static_cast<int> (std::llround (samplesFromBlockStart)));
        }

        EmitEvent ev = pending.event;
        ev.sampleOffset = sampleOffset;
        out.push_back (ev);
    }

    pendingEmits.swap (stillPending);
    hubMutex.unlock();

    return out;
}
