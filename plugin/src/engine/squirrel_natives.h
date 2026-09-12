#pragma once

// THE SCRIPT API, DUMPED AS DIRECTLY CALLABLE client.dll ADDRESSES.
//
// Every Squirrel native is a thin VM wrapper around an engine C++ function.
// Capturing the SQFuncRegistration structs as the client VM is built yields
// name -> signature -> function pointer for the entire script surface in one
// run, which is the map the pure-C++ endgame is ported against: the script mod
// proves WHICH call sequence works, this file says WHERE those calls live.
//
// This is investigation scaffolding on the plugin side. It reads and logs; it
// changes no engine state and drives nothing.

// Installs the registration hook. Must run before the client VM is created --
// registrations all happen during VM construction, so there is no later moment
// at which a hotkey could catch them. It is therefore INI-gated
// (`set natives.dump = 1`) rather than hotkey-gated: the standing rule that
// nothing arms on load is kept by defaulting to off.
void EnsureSquirrelNativeDumpInstalled(void* clientModule);

// Removes the patch. Safe to call when nothing was installed.
void RemoveSquirrelNativeDump();

// Writes %TEMP%\tf2vr-natives.txt once the registration count has stopped
// growing. Called every frame; does nothing until there is something complete
// to write, and nothing again afterwards.
void PollSquirrelNativeDump();

// INI: `set natives.dump = 1`.
void SetSquirrelNativeDumpEnabled(bool enabled);
bool IsSquirrelNativeDumpEnabled();

// How many registrations have been captured so far, for status lines.
unsigned long long SquirrelNativeRegistrationCount();
