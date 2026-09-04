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

// Phase 3: Hub ppq-aware emit buffer. A Sender pushes each note on/off it
// observes (tagged with its own ppq) to the Hub over a small fixed-size
// binary message - unlike OrchCaptureLink's once-per-take JSON blob, this
// runs continuously for the life of the transport, so per-message overhead
// actually matters (see Docs/OrchMerge_Design.md SS6).
//
// The Hub does NOT relay immediately on receipt: a live 13-sender test (all
// senders share the same Bitwig transport, so ppq is a directly comparable
// shared timeline) showed genuine cross-instrument unisons arriving spread
// out by ordinary socket/thread relay jitter widely enough to land in
// SEPARATE onset groups at the downstream reducer, each of which correctly
// (from its own local view) kept and re-emitted the same pitch - confirmed
// by inspecting the actual captured MIDI, not guessed. hubMarginMs (SS3 of
// the design doc) holds each decided-to-emit event for that many ms of wall-
// clock slack, then maps its ORIGINAL ppq to a sample position in whatever
// block is playing by the time the margin elapses - same technique as
// OrchPiano's own flushPlanBuffer/emitSampleFor, just ms-scaled (absorbing
// relay jitter) rather than beat-scaled (a musical lookahead).
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
    // Called once per block with the block's own ppq mapping (or ppqPerSample
    // 0.0 if the host isn't reporting ppq - everything just emits at sample 0
    // then, same as Phase 2). Only returns events whose hubMarginMs has
    // elapsed AND whose target ppq has actually arrived in this block (an
    // event due before the transport gets there is simply held for a later
    // block - rare, only possible with a very short margin).
    struct EmitEvent
    {
        bool isNoteOn = true;
        int channel = 1, note = 60, velocity = 100;
        int sampleOffset = 0;
    };
    std::vector<EmitEvent> drainEmitEvents (double blockStartPpq, double ppqPerSample, int numSamples);

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

    // Hub role: connection threads -> processBlock. `holdCounts` is how many
    // holders currently sound a given (channel, note) - a NoteOn is only
    // relayed downstream when the count goes 0->1 (the first holder), and a
    // NoteOff only when it goes back to 0 (the last one releasing). This
    // gates BOTH directions: a note-off from one sender never silences a
    // still-sounding note from another (SS4 of the design doc), and - the
    // Phase 2 bug this fixes - a second sender (or the same sender's own
    // divisi, e.g. a paired "2 Oboi"/"2 Clarinetti" track) hitting a pitch
    // that's already sounding no longer produces a second stacked notehead
    // downstream; it just adds to the count. `bySender` is the per-sender
    // breakdown, needed both to force-release everything a Sender was
    // holding if its connection drops without a clean note-off, and to
    // correctly count a single sender holding the same (channel, note) more
    // than once (real divisi within one track).
    std::mutex hubMutex;
    std::map<std::pair<int, int>, int> holdCounts;
    std::map<std::string, std::map<std::pair<int, int>, int>> bySender;
    std::map<HubConnection*, std::string> connectionToSender;
    std::vector<std::unique_ptr<HubConnection>> hubConnections;

    // An event that has passed the holdCounts gate above still waits here
    // until hubMarginMs of real time has elapsed since it arrived - see
    // drainEmitEvents(). A force-release from a dropped connection uses
    // kImmediatePpq so it emits on the very next block regardless of margin
    // (there is no "correct" musical position for an out-of-band release).
    struct PendingEmit
    {
        juce::int64 readyAtMs = 0;
        double ppq = 0.0;
        EmitEvent event;
    };
    std::vector<PendingEmit> pendingEmits;

    JUCE_DECLARE_WEAK_REFERENCEABLE (OrchMergeLink)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchMergeLink)
};
