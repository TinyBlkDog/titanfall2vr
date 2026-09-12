#pragma once

// ---------------------------------------------------------------------------
// THE TITAN-STATE READ. KICKOFF-TITAN-ARMS-2026-09-03.
//
// A POLLED STATE, not a detected event. Every plugin frame this asks the local
// player entity the engine's own question -- C_Player::IsTitan() -- and derives
// embark/disembark edges from the answer changing. A polled boolean fails safe:
// a frame it cannot read is "unknown", never a spurious transition.
//
// HOW THE READ WAS FOUND (pescan, offline, selftest 0.9872% bad on client.dll):
//
//   IsTitan  Squirrel native   client.dll+0x3DDC00
//       call 0x12F80           sq_getthisentity -- unwrap + validate
//       call 0x3DA768          mov rax,[rcx] ; jmp [rax+0x4B0]   <- vtable slot
//   IsPlayer Squirrel native   client.dll+0x3DDBA0
//       call 0x3DA75C          mov rax,[rcx] ; jmp [rax+0x4A8]   <- vtable slot
//
// On the C_Player vtable (0x8C7A58, the same one the eye hook swaps):
//   +0x4A8 -> 0x1576D0  C_Player::IsPlayer   mov al,1 ; ret
//   +0x4B0 -> 0x2C92F0  C_Player::IsTitan    class record [this+0x1E60]
//                                            -> +0x272C class id in {1,3}
//
// So the read is two virtual calls on GetLocalPlayer(), exactly the shape the
// height fix used to unwrap ScriptEyePosition (RESULT-HEIGHT-2026-09-02 §3).
// IsPlayer is the POSITIVE CONTROL: it must read true on the local player in
// every state, so a false there means the entity or the slot is wrong and the
// IsTitan answer beside it is void. The instrument says so, loudly.
//
// The previous discriminator -- a call counter on the cockpit sway at
// client.dll+0x64F7B0 -- was REFUTED 2026-09-03: zero calls across a full run
// that included being inside a titan. It, its asm interceptor and its
// displaced-byte patch are gone.
//
// GATING. `titan.gate` (ini, DEFAULT 1 -- proven, like eye.hook):
//   1  the ARMS follow it: collapse stands down the frame the raw reading says
//      titan, re-arms once the debounced reading says on foot.
//   0  read-only, the diagnostic control arm. Logged, nothing acts.
//      IsInTitanNow() reads on-foot and TitanStateSettled() keeps the old
//      1.5 s-after-world timing.
// The eye raise does NOT follow the state: it applies in the cockpit too, and
// the wearer confirmed the cockpit height with it applied (a level-2 gate that
// stood it down was built, never needed, and deleted).
//
// RUNG ONE PASSED 2026-09-03 (content 16DF9CCA, dll e87bc554): edges at
// 27.2 s ON FOOT, 74.5 s IN TITAN, 109.2 s ON FOOT against the wearer's
// "out, in, out, about a minute each"; IsPlayer true on all 6546 polls that
// had a player, 0 faults; class index 18 on foot / 36 in the titan; the vptr
// stayed C_Player throughout.
//
// RUNG TWO PASSED 2026-09-03 (content 2EF203DD, dll 1dc8339d, gate 1): four
// to five titan entries, body hidden on foot every time, shown in the titan,
// no crash. The stand-down on the RAW reading lands 10-25 ms before the
// debounced edge and before the boarding animation. Gate 2 (the eye raise)
// was never needed: raise 8 applied inside the cockpit read right to the
// wearer across every run, so the eye stays on gate 1's behaviour.
// See docs/RESULT-TITAN-ARMS-2026-09-03.md.
// ---------------------------------------------------------------------------

// Once per plugin frame. Resolves and verifies the vtable slots on first call
// (refuses loudly and latched on any mismatch), polls the state, logs every
// edge uncapped and a heartbeat every 5 s with its own freshness counters.
void AdvanceTitanProbe(bool worldIsRendering);

// IN A TITAN RIGHT NOW, for the ARMS. False unless titan.gate >= 1. True the
// moment the raw reading says titan (control passing), and held while the
// debounced one does. Fails safe to "on foot", the behaviour that has been
// played for weeks.
bool IsInTitanNow();

// May a consumer ACT on the state? Gate 0: true 1.5 s after a world is up
// (the legacy timing, unchanged). Gate >= 1: true only while the reading is
// KNOWN -- a player is live, IsPlayer reads true, and the debounce has
// settled -- so a save that starts inside a titan never gets on-foot
// behaviour applied to a titan's model for its first frames.
bool TitanStateSettled();

// INI: `set titan.gate = 0|1`. See the header note.
void SetTitanGate(int level);
