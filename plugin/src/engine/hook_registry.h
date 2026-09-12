#pragma once

#include <cstddef>

// ---------------------------------------------------------------------------
// P0-c -- THE HOOK-SITE REGISTRY. PLAN-TITAN-2026-08-28 section 2.
//
// The prev-8 crash record ends at `unknown+0x0`: a faulting PC in executable
// memory that belongs to no loaded module. This plugin allocates exactly that
// kind of memory -- detour trampolines -- and nothing aggregated WHERE. So the
// record could say "one of ours, probably" and could not say WHOSE, which is
// the difference between one bisection run and five.
//
// Every site that allocates a trampoline or writes a detour into module code
// registers {name, address, length} here at install time. The crash recorder
// consults the registry from its handler:
//
//   - PC in no module, registry hit  -> the line names the owning hook instead
//     of `unknown`.
//   - PC inside a module, registry hit -> the line still carries module+RVA and
//     appends that the bytes at that RVA are OURS (a patched site).
//
// WHAT IS DELIBERATELY NOT REGISTERED: pointer swaps (vtable slots, IAT
// entries). A swapped pointer is data -- a faulting PC never lands IN it; it
// lands in the interceptor the pointer targets, which is inside this DLL and
// already resolves by module. Registering them would widen the registry's
// meaning from "executable bytes we created or modified" to "things we
// touched", and a registry that answers a sharper question is worth more.
//
// HANDLER SAFETY. Registration happens at install time on ordinary threads;
// lookup happens inside a vectored exception handler in a faulted process. So
// the lookup takes no lock, allocates nothing and calls nothing: it walks a
// fixed array of entries published with a release store and counts published
// entries with an acquire load. Names must be string literals (the registry
// stores the pointer, not a copy).
// ---------------------------------------------------------------------------

// Records one executable range. Idempotent per address: re-registering the same
// address updates nothing and adds nothing, so re-installs cannot flood the
// table. `name` MUST be a string literal or otherwise immortal.
void RegisterHookSite(const char* name, const void* address, std::size_t length);

// Handler-safe lookup: returns the owning entry's name and optionally its base
// and length, or nullptr when no registered range covers `pc`.
const char* LookupHookSite(const void* pc, const void** baseOut, std::size_t* lengthOut);

// How many entries are published. For banners and the self-check line.
int HookRegistryCount();

// The offline halves of P0-c's falsifier pair, run once at startup and printed:
//
//   POSITIVE: a registered range's own address must resolve through
//     LookupHookSite to its name (proves the lookup can hit at all -- an
//     elimination instrument that has never fired is not evidence).
//   NEGATIVE: an address inside titanfall2vr.dll (this function's own) must
//     NOT match any entry -- the crash.selftest deliberate fault has its PC in
//     this DLL, and it must keep resolving to the module, never to a registry
//     entry.
//
// The remaining live half -- a real fault in a trampoline naming its owner --
// can only be proven by a real crash, and the line says so.
void HookRegistrySelfCheck();
