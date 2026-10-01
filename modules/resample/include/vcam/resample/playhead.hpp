// =============================================================================
// playhead.hpp — which source frame is shown in output slot n?
//
// The output clock (slots n = 0, 1, 2, ... at the output FPS) and the source's
// own time (frame timestamps) are separate (docs/ARCHITECTURE.md §1.3, §7).
// The Playhead links them with an ANCHOR "(slot n_a shows source time s_a)":
//
//     source_time(n) = s_a + (n − n_a) · (1 / output_fps)
//
// and then picks the source frame on screen at that time (HOLD: the last frame
// whose pts <= source_time — what a player shows; or NEAREST).
//
// Pause, resume, seek and loop only move the anchor, so the OUTPUT cadence
// never jumps (§7.1):
//   pause(n)      freeze source time            resume(n)  anchor = (n, frozen time)
//   seek(n, t)    anchor = (n, t)               loop       anchor = (n, time − duration)
//
// Exactness: all times use SourceClock's integer units, in which one source
// tick and one output period are both whole numbers. So 30 → 30 FPS maps slot
// n to frame n exactly, forever; no float rounding can make it pick frame n−1.
//
// Cost per slot: amortised O(1) (a cursor moves forward); after a seek/loop
// one O(log N) binary search.
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>

#include "vcam/buffer/timeline.hpp"
#include "vcam/core/rational.hpp"
#include "vcam/core/status.hpp"
#include "vcam/resample/source_clock.hpp"

namespace vcam {

enum class SelectionPolicy {
    Hold,     // frame being displayed at that instant (default)
    Nearest,  // frame whose timestamp is closest
};

enum class EofPolicy {
    Stop,  // stop streaming after the last frame's display time
    Loop,  // continue from frame 0 with unchanged output cadence
    Hold,  // keep showing the last frame forever
};

const char* eof_policy_name(EofPolicy policy);
const char* selection_policy_name(SelectionPolicy policy);
Result<EofPolicy> parse_eof_policy(const std::string& text);
Result<SelectionPolicy> parse_selection_policy(const std::string& text);

// What to show in one slot.
struct Selection {
    uint64_t source_index = 0;
    bool end_of_stream = false;  // Stop policy reached the end: nothing to show
    bool at_end = false;         // Hold policy: past the end, showing the last frame
    bool looped = false;         // wrapped to the beginning in this slot
    bool repeated = false;       // same source frame as the previous slot
    uint32_t skipped = 0;        // source frames jumped over since the previous slot
};

class Playhead {
public:
    // `timeline` must stay alive and unchanged while the Playhead is used.
    Playhead(const Timeline& timeline, Rational output_fps, SelectionPolicy selection, EofPolicy eof);

    // Slot `first_slot` shows source time 0.
    void start(uint64_t first_slot);

    // Frame for slot `slot`. Call with non-decreasing slot numbers.
    Selection select(uint64_t slot);

    void pause(uint64_t slot);
    void resume(uint64_t slot);
    bool paused() const { return clock_.paused(); }

    // Jump to source time `seconds` from slot `slot` on. Times outside the
    // stream are clamped to [0, last frame].
    Status seek(uint64_t slot, double seconds);

    // Source time (seconds) shown at `slot`, for status displays.
    double position_seconds(uint64_t slot) const;
    double duration_seconds() const;
    uint64_t loops() const { return loops_; }

private:
    uint64_t index_for_position(Int128 position);

    const Timeline& timeline_;
    SelectionPolicy selection_policy_;
    EofPolicy eof_policy_;

    SourceClock clock_;       // anchor / pause / seek arithmetic (source_clock.hpp)
    Int128 end_units_;        // stream length in clock units

    size_t cursor_ = 0;                    // last selected frame (search hint)
    std::optional<uint64_t> previous_;     // frame selected in the previous slot
    uint64_t loops_ = 0;
};

}  // namespace vcam
