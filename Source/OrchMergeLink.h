#pragma once

#include <atomic>
#include <memory>
#include <JuceHeader.h>

class OrchMergeAudioProcessor;

// Phase 1: Sender-side discovery/connect skeleton only. No Hub, no message
// payload yet (the Sender->Hub wire format is an open question for Phase 2 -
// see Docs/OrchMerge_Design.md SS6).
//
// ALL socket work runs on this object's own background thread - NEVER the host
// message thread - because a real template runs ~25 Sender instances plus one
// Hub, and a blocking connect on each, on the one shared message thread,
// freezes the host (feedback_no_message_thread_ipc_at_scale; OrchCaptureLink
// hit this at ~50-100 instances before it was moved off the message thread).
//
// Discovery is via a lock file: the Hub (once built, Phase 2) will write
// <temp>/orchmerge-hub.lock while it holds port 47828. A Sender only attempts
// a socket connection when that file exists, so the common "no Hub in the rig
// yet" state costs one File::existsAsFile() per poll and nothing else - safe
// to drop this plugin onto a track before the Hub exists.
//
// `role` parameter Sender (default) -> this class tries to connect to the Hub.
// `role` parameter Hub -> Phase 1 does not implement the Hub side at all;
// this class just reports HubNotBuilt so the editor can say so.
class OrchMergeLink : private juce::Thread
{
public:
    explicit OrchMergeLink (OrchMergeAudioProcessor&);
    ~OrchMergeLink() override;

    static constexpr int kPort = 47828;

    enum class Mode { Sender, HubNotBuilt };

    // ---- thread-safe queries for the editor (message thread) ----
    Mode getMode() const { return mode.load(); }
    bool isSenderConnected() const { return senderConnected.load(); }

private:
    class SenderConnection;

    void run() override;
    void reconcileMode();
    void serviceSender();
    static juce::File lockFile();

    OrchMergeAudioProcessor& processor;

    std::atomic<Mode> mode { Mode::Sender };
    std::atomic<bool> senderConnected { false };

    // owned + touched only by the worker thread (run())
    std::unique_ptr<SenderConnection> sender;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrchMergeLink)
};
