#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <JuceHeader.h>

class OrchMergeAudioProcessor;

// Phase 2: Hub naive merge. A Sender pushes each note on/off it observes
// (tagged with its own ppq) to the Hub over a small fixed-size binary
// message - unlike OrchCaptureLink's once-per-take JSON blob, this runs
// continuously for the life of the transport, so per-message overhead
// actually matters (see Docs/OrchMerge_Design.md SS6). The Hub relays
// immediately on receipt - there is no ppq-aware emit buffer yet, that is
// Phase 3 (SS3 of the design doc).
//
// ALL socket work stays off the host message thread (feedback_no_message_
// thread_ipc_at_scale / OrchCaptureLink's own Phase-2 lesson): connection
// management runs on this object's own background juce::Thread, and every
// InterprocessConnection is constructed with callbacksOnMessageThread=false
// so message parsing runs on the connection's own thread. Shared state
// touched from those connection threads is guarded by hubMutex; processBlock
// (audio thread, both roles) never blocks on it - see drainEmitEvents().
//
// Discovery is via a lock file: the Hub writes <temp>/orchmerge-hub.lock
// while it holds port 47828. A Sender only attempts a socket connection when
// that file exists, so the common "no Hub in the rig yet" state costs one
// File::existsAsFile() per poll and nothing else - safe to drop this plugin
// onto a track before the Hub exists.
class OrchMergeLink : private juce::Thread
{
public:
    explicit OrchMergeLink (OrchMergeAudioProcessor&);
    ~OrchMergeLink() override;

    static constexpr int kPort = 47828;

    enum class Mode { Sender, Hub, HubPortBusy };

    // ---- thread-safe queries for the editor (message thread) ----
    Mode getMode() const { return mode.load(); }
    bool isSenderConnected() const { return senderConnected.load(); }
    int getConnectedSenderCountForUi() const { return connectedSenderCount.load(); }
    juce::int64 getLastRelayedNoteMsForUi() const { return lastRelayedNoteMs.load(); }

    // ---- Sender role: the audio thread pushes what it observes here ----
    // Called from OrchMergeAudioProcessor::processBlock only when this
    // instance's role is Sender. Never blocks the caller for long - a
    // SpinLock-guarded queue the worker thread drains and forwards over the
    // socket a few milliseconds later.
    struct NoteEvent
    {
        bool isNoteOn = true;
        int channel = 1, note = 60, velocity = 100;
        double ppq = 0.0; // this Sender's own timeline - not yet used for
                           // placement anywhere (Phase 3), carried now so the
                           // wire format doesn't need to change later.
    };
    void pushOutgoingEvent (const NoteEvent&);

    // ---- Hub role: processBlock drains what's ready to emit right now ----
    // Phase 2 is naive: every event a connection thread has decided to relay
    // (after the note-off gating in SS4 of the design doc) is ready for the
    // very next block - there is no ppq-aware placement yet.
    struct EmitEvent
    {
        bool isNoteOn = true;
        int channel = 1, note = 60, velocity = 100;
    };
    std::vector<EmitEvent> drainEmitEvents();

private:
    class SenderConnection;
    class HubConnection;
    class HubServer;

    void run() override;
    void reconcileMode();
    void serviceSender();
    void teardownHub();
    static juce::File lockFile();

    void registerHubConnection (std::unique_ptr<HubConnection>);
    void onHubMessage (HubConnection*, const juce::MemoryBlock&);
    void onHubConnectionLost (HubConnection*);
    void releaseAllForSender (const std::string& senderKey);
    friend class SenderConnection;
    friend class HubConnection;
    friend class HubServer;

    OrchMergeAudioProcessor& processor;
    const juce::Uuid localUid;

    std::atomic<Mode> mode { Mode::Sender };
    std::atomic<bool> senderConnected { false };
    std::atomic<int> connectedSenderCount { 0 };
    std::atomic<juce::int64> lastRelayedNoteMs { 0 };

    // owned + touched only by the worker thread (run())
    std::unique_ptr<SenderConnection> sender;
    std::unique_ptr<HubServer> hubServer;
    bool wroteLockFile = false;

    // Sender role: audio thread -> worker thread.
    juce::SpinLock outgoingLock;
    std::vector<NoteEvent> outgoingEvents;

    // Hub role: connection threads -> processBlock. `holders` tracks who is
    // currently sounding a given (channel, note) so a note-off from one
    // sender never silences a still-sounding note from another sender that
    // happens to share the same output channel/pitch - the anonymous-merge
    // correctness problem SS4 of the design doc calls out as the real risk
    // here (timing jitter, by contrast, is cosmetic until Phase 3).
    // `bySender` is the reverse index, needed to force-release everything a
    // Sender was holding if its connection drops without a clean note-off.
    std::mutex hubMutex;
    std::map<std::pair<int, int>, std::set<std::string>> holders;
    std::map<std::string, std::set<std::pair<int, int>>> bySender;
    std::map<HubConnection*, std::string> connectionToSender;
    std::vector<std::unique_ptr<HubConnection>> hubConnections;
    std::vector<EmitEvent> readyToEmit;

    JUCE_DECLARE_WEAK_REFERENCEABLE (OrchMergeLink)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchMergeLink)
};
