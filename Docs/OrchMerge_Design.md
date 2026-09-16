# OrchMerge — Design & Build Plan

**Status: SCOPING, no code yet.** Written before any code, mirroring `OrchHarp_Design.md` /
`OrchPiano_Design.md`. Repo `C:\AudioDev\Repos\OrchMerge` (created, not git-init'd).

## 0. The problem

OrchPiano's **Reduce** mode (a whole orchestral score → one piano part) needs to see every
orchestral track's notes at once. OrchPiano is a plain MIDI effect — it reads whatever it's fed —
so this is purely a **routing** problem: get ~25 tracks' note streams onto the one track OrchPiano
sits on.

Bitwig has no reliable built-in way to do this. Researched 2026-09-04 (KVR/Bitwig forum threads):
Note-Receiver-stacking-in-a-Layer and Group-track auto-merge each have a report saying they work
and a report saying they don't — version-dependent, unverifiable, not something to build a
template around. The one concretely-described working method ("Notes to Tracks" per-source
routing) may work but is unconfirmed at 25-track scale and is native-Bitwig-version-dependent
regardless.

**Decision: build it ourselves**, reusing this ecosystem's own proven IPC pattern (MC↔MPL's
`PatternSyncServer`, OrchCapture's `OrchCaptureLink` coordinator) rather than depending on Bitwig's
routing. A one-time template investment: senders on every source track, one hub before OrchPiano.

## 1. Scope

- One plugin, **`OrchMerge`**, code `Omrg`, VST3, `IS_MIDI_EFFECT`.
- One `role` param: **Sender** (default) / **Hub**. ~25 instances run as Sender, one as Hub —
  exactly OrchCaptureLink's own Client/Coordinator toggle, same shape.
- **Anonymous merge** — the Hub relays a plain merged note stream; it does **not** expose which
  source track a note came from downstream. OrchPiano's reduction doesn't use source identity
  (§4.3 explains why this is fine, and the one place it still matters internally).
- **Not** in scope: audio routing (this is Note/MIDI only), keyswitch/articulation awareness
  (OrchMerge doesn't know what a keyswitch is — every note byte is just relayed), any musical
  judgement (that's OrchPiano's job entirely).

## 2. Architecture

```
 Track 1 (Vln I)  ──▶ [OrchMerge: Sender] ──┐
 Track 2 (Vln II) ──▶ [OrchMerge: Sender] ──┤
 ...  (~25 tracks)                          ├──▶ local socket ──▶ [OrchMerge: Hub] ──▶ OrchPiano
 Track 25 (Cb)    ──▶ [OrchMerge: Sender] ──┘         (off audio thread, both ends)
```

- **Sender**: transparent pass-through (never alters its own track's MIDI, same invariant as
  OrchCapture — "this plugin only observes/relays"). An off-thread `juce::Thread` (never the
  audio or message thread — `feedback_no_message_thread_ipc_at_scale`: a per-instance blocking
  connect on the message thread with ~25+ instances freezes the host) pushes each note-on/off it
  sees, tagged with the **sender's own `getPlayHead()` ppq** at the moment of the event, to the
  Hub. Discovery: only attempts a connection when the Hub's lock file exists (near-zero cost with
  no Hub present, same as OrchCaptureLink).
- **Hub**: binds the socket, accepts all Sender connections, merges their pushed events by ppq
  (see §3 — this is the one genuinely new piece, not just a copy of OrchCaptureLink), and emits an
  ordinary merged MIDI stream from its own `processBlock`. **OrchPiano needs zero changes** — it
  just sees normal MIDI arriving on whatever track the Hub sits on.
- **Port**: `47828` (after MPL IPC 47823, MC↔MPL/computer-use bridge 47824, transport-companion
  OSC 47825, OrchCapture coordinator 47826 — **verify no collision with anything else live before
  first connect**, this list is from memory, not a fresh port scan).
- **Lock file**: `<temp>/orchmerge-hub.lock`, same discovery pattern as OrchCaptureLink.

## 3. The one new problem: re-deriving timing on the Hub side

A note pushed by a Sender arrives at the Hub after socket + worker-thread relay latency — call it
tens of milliseconds, not sample-accurate. If the Hub just emitted everything it had drained at
sample 0 of its current block, two things could go wrong: (a) notes that were truly simultaneous
across two source tracks could land in different blocks and lose their simultaneity, or worse
(b) a wider drain batch could flatten genuinely different onsets into one instant.

**Fix: a small ppq-aware emit buffer on the Hub, same technique as OrchPiano's own
`flushPlanBuffer`/`emitSampleFor`, just with a much smaller margin.** The Hub holds each received
event until `hubMarginMs` (default **60 ms**, range 10–500) of *relay slack* has passed, then maps
its original ppq to a sample position in the Hub's current block exactly like OrchPiano does for
its own lookahead - the *musical* relationships between merged notes are preserved; only a small,
constant, deliberate delay is added.

This margin is a **latency-jitter absorber**, not a musical lookahead — it's why it's parameterised
in milliseconds, not beats (an IPC relay hiccup doesn't scale with tempo the way phrase-level
lookahead does).

**2026-09-04 correction (Phase 3b): the readiness *gate* itself must be ppq-based, not wall-clock.**
The first Phase-3 cut (`820c9de`) computed `readyAtMs = juce::Time::currentTimeMillis() + hubMarginMs`
at the moment each message arrived on its own connection thread — so two genuinely-simultaneous
cross-sender notes, delayed by different amounts of ordinary socket/thread relay jitter before
reaching `onHubMessage`, could become *ready* in two different Hub audio blocks purely from that
arrival-order noise, not from anything musical. Once that happened, the later-draining one's sample
offset (computed from `(pending.ppq - blockStartPpq)`) would land in a later block than the first,
reintroducing exactly the split-onset symptom this phase exists to prevent — with no wrong note
count, so it would not show up as a simple note-diff, only as timing/onset-grouping drift.

Fixed (`e21a803`) by converting `hubMarginMs` into an equivalent number of **beats** at the Hub's
current tempo (`OrchMergeLink::setCurrentBpm`, fed each block from the same `bpm` the Hub's
`processBlock` already computes) and gating `PendingEmit::readyAtPpq = event.ppq + marginBeats`
against the block's own ppq window instead of against wall-clock time. Two simultaneous notes now
become ready together regardless of relay jitter, because readiness is measured on the same shared
musical timeline the sample-offset placement always used. Not yet Bitwig-tested at scale (the
original 13-sender jitter symptom that motivated Phase 3 was diagnosed away from this exact
mechanism only speculatively — the real root cause turned out to be OrchPiano's own onset-group
scan, `78c0136` — so this fix closes a real, code-grounded gap but is not yet confirmed to be
audible in practice).

### 3.1 Combined delay — a deliberately accepted simplification

The Hub adds `hubMarginMs` of delay on top of whatever `lookaheadBeats` OrchPiano itself adds
downstream. OrchPiano's existing delay-compensation CC (Phase 5d) reports only its *own* delay,
not the Hub's. **v1 does not close this gap** — `hubMarginMs` is tens of milliseconds, well inside
typical notation-quantize grid width (a 1/32 note at customary tempos is itself tens of ms), so the
residual timing error is expected to be inaudible/immaterial for the notation use case. Flagged
here rather than silently ignored; revisit only if a real test shows it matters.

## 4. Note-off correctness (the real design risk, not the timing one)

25 senders, potentially overlapping notes, and the Hub exposes an **anonymous** merged stream — so
how does a note-off get routed to the right note-on if two different tracks happen to sound the
same pitch at overlapping times?

- **Internally, the Hub tracks note lifecycles keyed by `(senderUid, channel, note)`** — it always
  knows unambiguously which live note a given sender's note-off belongs to, using the same
  FIFO-per-key tracking pattern already proven in this ecosystem (ONF's `TrackedNote` vector,
  OrchPiano's `activeNotes`). This bookkeeping is real and not optional, even though nothing about
  it is exposed downstream.
- **What it emits downstream** is a plain note-on/note-off pair per event — no channel remapping
  scheme is needed for correctness, because the Hub (not the receiver) is what disambiguates.
- **Same-pitch collisions across two tracks are not a problem for OrchPiano specifically** — its
  own `reduceGroup` already collapses simultaneous same-pitch note-ons from an onset group down to
  one (keeps the louder), which is the musically correct behaviour for a reduction anyway (§4.3).
  The Hub's internal correctness (never losing/misrouting a note-off) is what matters; a downstream
  consumer *seeing* two simultaneous identical pitches merge into one is a feature, not a bug, for
  this specific use case.
- **What the Hub does NOT need**: per-sender output channel assignment, voice-stealing, or any
  identity preserved past its own internal tracking table. Keeps it simple.

### 4.1 Why anonymous is fine (and the one place identity almost mattered)

OrchPiano's importance/role logic works from pitch, velocity, duration and register — never from
MIDI channel or source identity. The one place source identity *could* have mattered is §4.1's
`melodyChannels`/`bassChannels` source hints from the original Phase 5 plan — those were already
dropped (P5d re-scope) precisely because they depended on a structured multi-lane input OrchMerge
deliberately does not provide. If a future need for per-instrument-family hints emerges, it would
need a different, tagged protocol - out of scope for v1.

## 5. Phased build (proposed)

| Phase | Scope | Verifies |
|---|---|---|
| **1 — plugin skeleton, Sender pass-through only** | `role` param, transparent MIDI pass-through in both roles (so it's safe to drop onto a track before the Hub exists), lock-file discovery, off-thread socket connect (Sender side only; Hub role present but not yet implemented - reports "not built" in its status). | Loads, never alters the track's MIDI even mid-development. |
| **2 — Hub: naive merge** | Hub binds the port, accepts N senders, relays events **without** the ppq-aware emit buffer (emit immediately on receipt) - a known-simplified first cut. | End-to-end: 2-3 senders → one merged stream reaches OrchPiano; correctness of note-off routing (§4) under real overlapping notes. |
| **3 — Hub: ppq-aware emit buffer** | `hubMarginMs`, the small lookahead-style buffer (§3). | Simultaneous notes across tracks land together; no flattening of genuinely different onsets. |
| **4 — scale test + polish** | Wire up a real ~25-track template; editor status (connected sender count, last relayed note, buffer depth); decide whether `hubMarginMs` needs auto-tuning or stays a fixed knob. | The actual orchestral template, full scale. |

Each phase should get its own pure-logic check tool where there's pure logic to check (the ppq→
sample mapping in Phase 3 is the clearest candidate, mirroring OrchPiano's own `emitSampleFor`
tests) - most of Phases 1-2 are IPC plumbing with no meaningful pure-function surface, closer to
OrchCaptureLink than to OrchPianoReductionLogic in that respect.

## 5b. Transport-stop fix, `cb5ff74` (2026-09-05) - held notes never released

Found live, in Bitwig: a Hub instance showed a stuck-note activity indicator (a yellow
dot) with zero input while playback was stopped. Traced by reading the code, not
guessing - grepped this whole file for `wasPlaying`/`isPlaying`/`allNotesOff`/`panic`/
`releaseAll` and got zero matches before this fix. Two gaps combined:

- **Sender**: `processBlock` returns immediately whenever the host isn't playing, so a
  note still held at the exact instant the user hits Stop never gets its eventual
  note-off observed/forwarded to the Hub.
- **Hub**: never checked play-state at all - `drainEmitEvents` ran unconditionally every
  block, with no transport-stop handler anywhere.

Together: a note held at Stop sits in `holdCounts` forever - the visible stuck note. The
**invisible, worse half**: on the next take, a fresh note-on for that same pitch finds
`holdCounts` already non-zero, so `isFirstHolder` is false and the Hub silently treats the
new attack as "already sounding" instead of relaying it downstream - a real mechanism for
a missing-note symptom, not just a cosmetic stuck-light issue.

**Fix**: `OrchMergeLink::releaseAllHeld()` force-releases everything currently held, same
technique `releaseAllForSender` already used for one dropped connection, just for every
sender's holdings at once - clears `holdCounts`/`bySender` and emits an immediate
(`kImmediatePpq`) note-off for anything that was still counted. `OrchMergeProcessor` (Hub
role) now tracks `wasPlaying` and calls it on the exact playing→stopped edge. Builds
clean, reinstalled. **Not yet Bitwig-tested live** - next step is reproducing the original
stuck-note scenario and confirming both that the indicator clears AND that a subsequent
take's note-ons for the previously-stuck pitch aren't swallowed.

## 6. Open questions for the next session

- Exact wire format for Sender→Hub push: reuse OrchCaptureLink's `juce::var`/JSON-ish pattern, or
  something lighter given this needs to run continuously (not once per take)?
- Does the Hub need any UI at all beyond a status line (connected sender count), or is it truly
  fire-and-forget once wired?
- Confirm port 47828 is actually free (fresh scan, not memory recall) before first implementation.
- Should `role` default to Sender (safe - a freshly-dropped instance never accidentally tries to
  bind a port another instance already owns) - yes, matches OrchCaptureLink's own default
  (`coordinator` param OFF = Client).

## 5c. Sender keyswitch-zone exclusion, `61928fc` (2026-09-05)

Root-caused live via the actual full-rig capture: a 24-note cluster of near-duplicate
pitches 24-30 landed on OrchPiano's reduction channel at take-start - one
keyswitch/articulation-select firing per instrument, all together. Confirmed against the
real device chain (user's own screenshot): each Sender sits right after that track's own
OrchNoteMapper, which has already remapped the ecosystem's shared keyswitch **source**
zone (12-23) into **that instrument's own destination range** (seen live: flute lands in
24-33) before the Sender ever taps the signal. A downstream reducer with no keyswitch
concept at all (grepped OrchPiano's entire source - zero matches) has no way to tell these
apart from real notes.

Two things ruled out first, both by reasoning through the actual chain rather than
guessing: the per-track OrchCapture instance sitting *after* OrchPiano can't fix this (it
only affects what it exports, once OrchPiano has already reduced the contaminated input),
and even if it could, its own default KS zone (12-23) wouldn't match anyway - the
contamination is already past `OrchNoteMapper`'s remap, sitting in the destination range.

**Fix**: new Sender-only params, off by default (existing rigs see no behavior change) -
`excludeKsNotes` (bool), `ksZoneMin`/`ksZoneMax` (default 24-35, matching this ecosystem's
own "orchestra" destination convention, but genuinely **per-instance** since the actual
destination range differs by instrument - e.g. bass instruments land in 72-83 elsewhere in
this rig, per this ecosystem's own prior keyswitch-unification work). A note inside the
zone is simply never observed/forwarded to the Hub in `OrchMergeAudioProcessor::processBlock`'s
Sender branch - the real instrument downstream of that same Sender on that same track still
receives the note untouched either way, since Sender stays a transparent pass-through by
design. Builds clean, reinstalled. **Not yet Bitwig-tested live** - next step is enabling
this on the affected instrument tracks with each one's own actual destination range and
confirming the cluster is gone from a fresh capture.

## 5d. Delay Compensation CC - reporting Hub Margin downstream (2026-09-16)

Root-caused from a real OrchDelay session (a different repo, same rig): the user fed a clean
kick-start motif through a chain ending in an OrchMerge Hub, then captured the merged output with
OrchCapture for Dorico - and every note in the capture landed noticeably late, "giving Dorico a hard
time" in translation. Quantified directly from the actual captured `.mid` file (parsed by hand, not
guessed): the first note landed at tick 3916 of a 960-tick-per-quarter file at ~77bpm, 76 ticks past
the clean 4-beat/one-bar mark of 3840 - **61.7ms**, matching this plugin's own `hubMarginMs` default
(60ms) almost exactly.

This isn't a bug in Hub Margin itself - §3 explains why the Hub deliberately holds every event back
by this amount (absorbing Sender-to-Hub relay jitter so genuinely-simultaneous cross-instrument
unisons still land close enough together for a downstream reducer to group them as one onset). The
actual gap was that nothing downstream ever got told this delay was happening, so it just silently
shifted every captured note late with no way to correct for it.

**Fix**: reused a mechanism that already exists in this exact ecosystem rather than inventing a new
one - OrchPiano already reports its own constant lookahead delay (in beats) on a configurable CC
(`delayCompensationCc`, default CC 113), and OrchCapture already knows how to consume it
(`lookaheadCompensationCc`, same default 113): observes the CC, subtracts the reported beat-delay from
every captured note's onset/release, and stays otherwise fully transparent (the CC itself still passes
through untouched). OrchMerge's Hub now does the exact same thing: converts `hubMarginMs` to beats
using the CURRENT tempo (`hubMarginMs / 1000 * bpm / 60`, re-derived every send since a fixed
millisecond margin is a different number of beats at different tempos), and broadcasts it on the same
CC 113 by default - sent at transport start, on any value change, and re-sent every 16 beats as a
safety net for a late-loading listener, byte-for-byte the same cadence OrchPiano's own sender already
uses. Zero changes needed on OrchCapture's own side - it already treats any plugin's report on that CC
identically, regardless of which plugin sent it.

New param `delayCompensationCc` (0-127, default 113, 0=off), Hub role only. Builds clean, reinstalled.

**First attempt produced ZERO effect - real bug, not just "not yet live-tested"**: the user re-captured
with this enabled and the exported `.mid` landed at the exact same uncompensated tick as before. Root
cause: the "CC value = whole beats" wire format above suits OrchPiano's own lookahead (a deliberately
whole-beat planning window) but is fundamentally the wrong resolution for Hub Margin - at any realistic
tempo, 10-500ms is well under 0.5 beats (60ms at 77bpm is 0.077 beats), and `juce::roundToInt` on a
value that small always rounds down to **0**. Every "compensation" this Hub sent was, every time, a
report of zero delay - the bug was never in the subtraction math on OrchCapture's side, it was that a
whole-beats-only protocol has no resolution fine enough to carry a genuinely small delay at all.

**Real fix**: switched to a dedicated FINE-resolution CC instead of reusing OrchPiano's own whole-beats
one. Default changed 113→**114**, and the wire encoding changed from `beats` directly to `beats ×
kFineBeatScale` (64.0 - a constant that MUST match exactly on OrchCapture's own receiving side, see its
`OrchCaptureProcessor.cpp` for the twin constant and `lookaheadCompensationFineCc` param, added the same
session). 0-127 now covers 0-1.98 beats at ~0.0156-beat resolution - comfortably fine enough for Hub
Margin's whole realistic range. This does mean OrchCapture needed a real change after all (its own
Docs, §9, has the receiving-side half of this story) - the original claim of "zero changes needed
downstream" only held for the broken whole-beats version. Rebuilt + reinstalled both plugins.

**RESOLVED** - user re-captured and confirmed the first note now lands essentially exactly on the beat
(measured directly from the new `.mid`: 1 tick off out of 960, ~0.8ms). The same re-test also surfaced a
separate, unrelated real artifact (short "particle" notes from merging 3 independently-clocked
cross-instance-broadcast echo sources) - not a Hub Margin/delay-compensation issue at all; see
OrchCapture's own Docs §10 for that fix (the Hub's own unison-merge logic, §4 above, is working exactly
as designed here, not buggy).
