#pragma once

// Writes one short diagnostic line to both the debugger and the current
// user's temporary directory: %TEMP%\\titanfall2vr.log.
void Tf2VrLog(const char* message);
// Writes any buffered lines out immediately. For the crash path.
void Tf2VrLogFlush();
// Starts the 250 ms background flusher. Idempotent; call once at load.
void Tf2VrLogStartFlusher();
// Quiet by default: only failures, refusals, crashes and the run's identity are
// written. diag.verbose = 1 writes everything, which is the dev setting.
void Tf2VrSetLogVerbose(bool enabled);
bool Tf2VrLogVerboseEnabled();
// Bypasses the quiet filter entirely.
void Tf2VrLogAlways(const char* message);
// The shared implementation; `force` bypasses the quiet filter.
void Tf2VrLogWrite(const char* message, bool force);

// OutputDebugStringA on every line takes a SYSTEM-WIDE lock and a kernel round
// trip whether or not a debugger is attached. Off by default; turn it on only
// when something dies before the file is readable.
void Tf2VrSetLogDebugChannel(bool enabled);
