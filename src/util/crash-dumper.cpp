#include <silk/util/crash-dumper.h>

#include <silk/util/platform.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/wait.h>

// PR_SET_PTRACER lifts yama's restriction that a child may not ptrace its parent. Pass PR_SET_PTRACER_ANY, not
// the dumper's pid: a pid-specific grant locks out a sanitizer's own leak-detection tracer, which then cannot
// suspend our threads and reports false leaks. Values from <linux/prctl.h>, redefined here in case headers lag.
#ifndef PR_SET_PTRACER
#    define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#    define PR_SET_PTRACER_ANY (static_cast<unsigned long>(-1))
#endif

namespace silk
{

/**
 * Hang + crash diagnosis built on a dedicated dumper process forked at install time, before silk starts
 * any threads, so the dumper is a clean single-threaded process. On a crash signal (SIGSEGV / SIGABRT /
 * SIGBUS / SIGFPE / SIGILL) or the hang signal, the in-process handler does only async-signal-safe work:
 * it writes one byte to wake the dumper and waits for it to finish. The dumper attaches gdb to us, sources
 * crash-dumper.py (every OS thread's backtrace plus the silk fiber list), then exits; the parent then cores
 * (crash) or exits with exitCode (hang). No fork / exec / stdio runs in signal context.
 */
class CrashDumper final
{
public:
    void install(int dumpSignal, int exitCode) noexcept;

    /** The crash / hang handler body; reached from the static signal trampoline. */
    void handleSignal(int signalNumber) noexcept;

private:
    static constexpr int SCRIPT_COMMAND_SIZE = 4200;

    /**
     * Runs in the forked child: block until poked, then exec gdb to dump the parent. Returns the child's
     * exit code in the cases where exec does not take over (nothing to dump, or gdb absent).
     */
    int runDumper(int requestReadFd) noexcept;

    /** Build "source <dir-of-our-binary>/crash-dumper.py" into sourceCommand (no compile-time path). */
    void buildSourceCommand() noexcept;

    int dumpSignalNumber = 0;
    int dumpExitCode = 0;
    int requestWriteFd = -1;
    pid_t dumperPid = -1;
    volatile sig_atomic_t dumpInProgress = 0;
    char sourceCommand[SCRIPT_COMMAND_SIZE] = {};
};

// Singleton: a C signal handler can only reach the instance through file scope.
static CrashDumper crashDumper;

static void crashSignalTrampoline(int signalNumber) noexcept
{
    crashDumper.handleSignal(signalNumber);
}

void CrashDumper::install(int dumpSignal, int exitCode) noexcept
{
    dumpSignalNumber = dumpSignal;
    dumpExitCode = exitCode;

    buildSourceCommand();

    // The pipe the crash handler signals the dumper through: the dumper blocks on the read end, the handler
    // writes one byte to the write end to request a dump.
    int pipeFds[2];
    int r = pipe2(pipeFds, O_CLOEXEC);
    if (r != 0)
    {
        std::fprintf(stderr, "crash-dumper: could not create pipe: r=%d\n", r);
        return;
    }

    // Fork the dumper while still single-threaded - before silk starts its scheduler threads - so the child
    // is a clean process that only waits on the pipe and execs gdb. O_CLOEXEC keeps the pipe ends from leaking
    // into any process the parent later forks and execs, which would hold the dumper alive past the parent.
    pid_t installerPid = getpid();
    pid_t child = fork();
    if (child == 0)
    {
        close(pipeFds[1]);

        // Die with the parent even if the poke pipe's write end leaked elsewhere, so the dumper can never
        // outlive the test process and hold the harness's captured output pipe open past the process's exit.
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        if (getppid() != installerPid)
        {
            _exit(0); // parent exited between fork and prctl - the death signal was already missed
        }

        _exit(runDumper(pipeFds[0]));
    }

    close(pipeFds[0]);

    if (child < 0)
    {
        close(pipeFds[1]);
        std::fprintf(stderr, "crash-dumper: could not fork dumper process\n");
        return;
    }

    requestWriteFd = pipeFds[1];
    dumperPid = child;

    // Allow any tracer (the dumper, and a sanitizer's leak-detection tracer) past yama ptrace_scope >= 1.
    int p = prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    SILK_UNUSED(p);

    // Run the handler on an alternate stack so a stack-overflow SIGSEGV can still enter it; SA_RESETHAND
    // restores the default disposition before any re-entry on the same signal.
    static char alternateStack[64 * 1024];
    stack_t signalStack;
    signalStack.ss_sp = alternateStack;
    signalStack.ss_size = sizeof(alternateStack);
    signalStack.ss_flags = 0;
    sigaltstack(&signalStack, nullptr);

    struct sigaction action = {};
    sigemptyset(&action.sa_mask);
    action.sa_handler = crashSignalTrampoline;
    action.sa_flags = SA_ONSTACK | SA_RESETHAND;

    static const int crashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
    for (int signalNumber : crashSignals)
    {
        sigaction(signalNumber, &action, nullptr);
    }

    // Hang path: the dump signal (e.g. SIGQUIT from "timeout --signal=") runs the same handler, which then
    // exits with the watchdog code instead of re-raising.
    sigaction(dumpSignal, &action, nullptr);
}

void CrashDumper::handleSignal(int signalNumber) noexcept
{
    // A second fatal signal while a dump is running: give up dumping and let the default action take the
    // process down.
    if (dumpInProgress)
    {
        signal(signalNumber, SIG_DFL);
        raise(signalNumber);
        return;
    }
    dumpInProgress = 1;

    // Async-signal-safe only: poke the pre-forked dumper and wait for it to finish (it attaches gdb, dumps,
    // detaches, and exits). No fork / exec / stdio here.
    if (requestWriteFd >= 0 && dumperPid > 0)
    {
        char request = 'D';
        ssize_t written = write(requestWriteFd, &request, 1);
        SILK_UNUSED(written);

        // Wait out the whole dump; retry across EINTR so an interrupting signal cannot let us re-raise or
        // exit while gdb is still attached.
        int status = 0;
        while (waitpid(dumperPid, &status, 0) < 0 && errno == EINTR)
        {
        }
    }

    if (signalNumber == dumpSignalNumber)
    {
        // Hang path: the dump is done, exit with the watchdog code.
        _exit(dumpExitCode);
    }

    // Crash path: restore the default disposition and re-raise so the process still dies with the original
    // signal (core dump, 128 + signal exit code) rather than being swallowed.
    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

void CrashDumper::buildSourceCommand() noexcept
{
    // crash-dumper.py is installed next to our binary; resolve it from /proc/self/exe rather than a
    // compile-time absolute path.
    char exePath[4096];
    ssize_t length = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (length <= 0)
    {
        std::snprintf(sourceCommand, sizeof(sourceCommand), "source crash-dumper.py");
        return;
    }

    exePath[length] = '\0';
    char * lastSlash = std::strrchr(exePath, '/');
    if (lastSlash)
    {
        *lastSlash = '\0';
        std::snprintf(sourceCommand, sizeof(sourceCommand), "source %s/crash-dumper.py", exePath);
    }
    else
    {
        std::snprintf(sourceCommand, sizeof(sourceCommand), "source crash-dumper.py");
    }
}

int CrashDumper::runDumper(int requestReadFd) noexcept
{
    // The dump / terminal signals are often broadcast to the whole process group - a shell pipeline, or
    // "timeout --signal=" - which would kill the dumper (default action) before it can dump. Ignore them so
    // the dumper survives; it still exits on pipe EOF (parent gone) or after the dump. SIG_IGN carries across
    // the exec into gdb.
    static const int ignoredSignals[] = {SIGQUIT, SIGINT, SIGTERM, SIGHUP, SIGPIPE};
    for (int signalNumber : ignoredSignals)
    {
        signal(signalNumber, SIG_IGN);
    }
    signal(dumpSignalNumber, SIG_IGN);

    // Block until the parent's handler pokes us (one byte), or the parent exits normally and closes the
    // pipe (EOF) - in which case there is nothing to dump.
    char request = 0;
    ssize_t got = read(requestReadFd, &request, 1);
    if (got <= 0)
    {
        return 0;
    }

    const char banner[] = "\n=== crash-dumper: dumping process state via gdb ===\n";
    ssize_t bannerWritten = write(STDERR_FILENO, banner, sizeof(banner) - 1);
    SILK_UNUSED(bannerWritten);

    // gdb attaches to the parent and sources crash-dumper.py (OS-thread backtraces + the silk fiber list).
    // exec replaces us with gdb; when gdb exits the parent's waitpid returns.
    char parentPidText[16];
    std::snprintf(parentPidText, sizeof(parentPidText), "%d", getppid());

    const char * argv[] = {
        "gdb",
        "-p",
        parentPidText,
        "-batch",
        "-nx",
        "-ex",
        sourceCommand,
        nullptr,
    };
    execvp("gdb", const_cast<char * const *>(argv));

    // exec failed (gdb absent): nothing to dump, but never hang - return so the parent's waitpid returns.
    const char message[] = "crash-dumper: could not exec gdb\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    SILK_UNUSED(written);
    return 127;
}

void installCrashDumper(int dumpSignal, int exitCode) noexcept
{
    crashDumper.install(dumpSignal, exitCode);
}

} // namespace silk
