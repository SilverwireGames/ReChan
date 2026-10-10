#pragma once

// QA probes for the scripted PSP autoplay build (PSP_AUTOPLAY).
//
// LOG() is compiled out on every PSP mode except debug (NDEBUG), so the probes
// write through their own small logger instead: lines are formatted into an
// in-memory buffer and appended to AUTOPLAY_LOG.TXT (relative path, same file
// as the input heartbeat in pspGamepad::Poll) when the buffer fills and on
// QaFlush(). Nothing here exists outside PSP_AUTOPLAY builds; every call site
// must be wrapped in #if defined(PSP_AUTOPLAY).
//
// Line format: "QA t=<aiTick> <event> key=value ...", one event per line.

#if defined(RC_PLATFORM_PSP) && defined(PSP_AUTOPLAY)

#include "core.h"

// Appends one formatted line (newline added) to the in-memory QA buffer.
void QaLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Writes the buffered QA lines to AUTOPLAY_LOG.TXT (append) and empties the buffer.
void QaFlush();

// Called once per AI tick after AI::MoveThings: logs door state transitions and,
// every 300 ticks, a player/score snapshot. Read-only with respect to game state.
void QaAutoplayTick();

// Logs the current world level/petal (and player checkpoint state) with a tag,
// then flushes. Used around level/petal loads.
void QaLogWorld(const char* tag);

// Extra button mask (PSP_CTRL_*) to OR into the scripted input for this Poll
// frame. Not called yet: pspGamepad::Poll needs the one-line hook described in
// the QA notes. Never returns PSP_CTRL_SELECT.
u32 QaForcedButtons(u32 frame);

// Returns a printable name for a possibly-null C string.
const char* QaName(const char* name);

#endif
