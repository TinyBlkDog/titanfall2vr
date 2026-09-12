#pragma once
#include <cstddef>
#include <cstdint>

// Paths are separate identities: FC500's saved +68 and FC960's live +70.
// Returns true when this draw is to be suppressed.
bool RuiHuntObserve(unsigned path, std::uintptr_t target);
// F3. Advances the ladder one step, and starts it if it is not running.
void RuiHuntStep(std::uintptr_t uiBase);
// F5. "The friendly name vanished at THIS step" -- the same meaning at every
// step, so there is no mode for the wearer to keep track of.
void RuiHuntMark();
// F12. Restores everything and rebuilds the walk from the fuller census.
void RuiHuntReset();
bool RuiHuntVisible();
// True only once the ladder is actually running: what the on-screen prompt is
// gated on, so a play-normally run has no panel parked in the wearer's view.
bool RuiHuntPromptWanted();
void RuiHuntStatus(char* text, std::size_t capacity);
