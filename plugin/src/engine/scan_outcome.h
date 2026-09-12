#pragma once

// HOW A SCAN ENDED, AS A TYPE THE REPORT LINE CANNOT OMIT.
//
// Four different silent bounds -- a 512 MB byte cap, a 64-instance cap, a
// 0x600 pointer-depth cap, and a 4 s time budget -- each produced a confident
// wrong answer on 2026-08-17, because a truncated scan printed its results in
// a form indistinguishable from a complete one. Two runs of identical code
// even produced opposite answers, each seeing a different prefix of a moving
// heap.
//
// The rule this type enforces: every bounded loop exits through Finish(),
// recording WHICH bound ended it, and the report line prints Describe() in
// the same line as the counts. A zero without "COMPLETE" beside it is not an
// absence and the log says so on its face. New scans must use this; a scan
// that reports counts without a ScanOutcome is a defect by definition.

struct ScanOutcome {
    enum class End {
        NotRun,       // Finish() never called: the scan did not run to any end
        Complete,     // the whole search space was walked; a zero is a real absence
        ByteCap,      // stopped at the byte cap
        InstanceCap,  // an instance/target list filled up; later objects were dropped
        TimeBudget,   // stopped at the wall-clock budget
        DepthCap,     // stopped at an offset/indirection depth bound
        WalkFailed,   // VirtualQuery failed or the region walk stalled mid-way
    };

    End end = End::NotRun;

    // The FIRST bound to fire is the one that decided the scan; later calls
    // must not soften it. In particular a loop that hits a cap and then
    // reaches its natural end of iteration must still report the cap.
    void Finish(End how) {
        if (end == End::NotRun) end = how;
    }

    bool Complete() const { return end == End::Complete; }

    // Written to be embedded in the result line itself, so the decision about
    // whether a zero means anything sits beside the zero.
    const char* Describe() const {
        switch (end) {
            case End::Complete:
                return "COMPLETE -- the whole space was searched, so a zero here is a real absence";
            case End::ByteCap:
                return "TRUNCATED BY THE BYTE CAP -- unscanned memory remains, a zero here says NOTHING";
            case End::InstanceCap:
                return "TRUNCATED BY A FULL INSTANCE LIST -- later objects were dropped, a zero or a "
                       "missing object here says NOTHING";
            case End::TimeBudget:
                return "TRUNCATED BY THE TIME BUDGET -- the scan stopped early, a zero here says NOTHING";
            case End::DepthCap:
                return "TRUNCATED BY THE DEPTH BOUND -- deeper offsets were never looked at, a zero "
                       "here says NOTHING";
            case End::WalkFailed:
                return "ABORTED: THE REGION WALK FAILED MID-WAY -- coverage is unknown, a zero here "
                       "says NOTHING";
            case End::NotRun:
            default:
                return "NEVER FINISHED -- the scan did not record an ending, which is itself a defect";
        }
    }
};
