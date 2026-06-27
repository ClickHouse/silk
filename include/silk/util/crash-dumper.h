#pragma once

#include <signal.h>

namespace silk
{

/**
 * Install hang + crash diagnosis. Forks a dedicated dumper process that blocks until signalled; on a
 * crash signal (SIGSEGV / SIGABRT / SIGBUS / SIGFPE / SIGILL) or dumpSignal, the in-process handler does
 * only async-signal-safe work - it wakes the dumper over a pipe and waits. The dumper attaches gdb to us
 * and writes every OS thread's backtrace plus the silk fiber list to stderr; the process then cores (crash)
 * or exits with exitCode (hang). Running the dump from a separate, pre-forked process keeps the handler
 * signal-safe and survives a fiber-level deadlock.
 *
 * Call once, FIRST in main, BEFORE silk initialization: the dumper is forked here, while the process is
 * still single-threaded, so it is a clean child. Run under "timeout --signal=<dumpSignal>" so a wedged run
 * self-dumps. Requires crash-dumper.py and fiber.py installed next to the binary, which the build does
 * automatically.
 */
void installCrashDumper(int dumpSignal = SIGQUIT, int exitCode = 124) noexcept;

} // namespace silk
