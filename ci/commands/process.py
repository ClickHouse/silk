import logging
import os
import socket
import subprocess
import sys
import time
from typing import Any

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

log = logging.getLogger("bb")


def _with_timeout(args: tuple[str, ...], kwargs: dict[str, Any]) -> tuple[str, ...]:
    """Turn a timeout= kwarg into a timeout command prefix that sends SIGQUIT, so a hung silk binary is caught by
    its in-process crash dumper (threads, fibers, sleeps, rings) before the SIGKILL after the grace period; args
    stay as they are without a timeout.
    """
    seconds = kwargs.pop("timeout", None)
    if not seconds:
        return args
    return ("timeout", "--signal=QUIT", "--kill-after=60", str(seconds)) + args


def run(*args: str, **kwargs: Any) -> None:
    """Run args to completion in ROOT; a non-zero exit raises CalledProcessError."""
    args = _with_timeout(args, kwargs)
    log.debug("run command: %s", " ".join(args))
    subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def run_capture(*args: str, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    """Run args in ROOT capturing stdout and relaying stderr; a non-zero exit raises CalledProcessError."""
    args = _with_timeout(args, kwargs)
    log.debug("run command: %s", " ".join(args))
    result = subprocess.run(
        args, cwd=ROOT, capture_output=True, text=True, check=False, **kwargs
    )
    if result.stderr:
        sys.stderr.write(result.stderr)
    result.check_returncode()
    return result


def start_process(*args: str, **kwargs: Any) -> subprocess.Popen[str]:
    """Start args in ROOT and hand it back once it has outlived its first 100 ms; an earlier exit raises, whatever
    its status: a caller wants a live process, and a server or profiler that returned already has none.
    """
    proc = subprocess.Popen(args, cwd=ROOT, text=True, **kwargs)
    log.debug("run command: %s", " ".join(args))
    try:
        deadline = time.monotonic() + 0.1
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                log.error(
                    "exited within 100 ms: r=%d %s", proc.returncode, " ".join(args)
                )
                raise subprocess.CalledProcessError(proc.returncode, args)
            time.sleep(0.01)
    except BaseException:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        raise
    return proc


def start_server(
    *args: str, host: str, port: int, **kwargs: Any
) -> subprocess.Popen[str]:
    """Start args with start_process and hand it back once host:port accepts a TCP connection; a server that
    does not listen within 5 s is killed and fails the run.
    """
    server = start_process(*args, **kwargs)
    try:
        _wait_for_tcp_port(host, port)
    except BaseException:
        server.kill()
        server.wait()
        raise
    return server


def _wait_for_tcp_port(host: str, port: int, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    log.error("%s:%d did not accept connections within %.0fs", host, port, timeout)
    sys.exit(1)


def cpu_split(server_cpus: str = "", client_cpus: str = "") -> tuple[str, str]:
    """The taskset lists for a server and its client: the given ones, else the lower and upper halves of the
    machine.
    """
    cpu_count = os.cpu_count() or 2
    half = max(1, cpu_count // 2)
    return server_cpus or f"0-{half - 1}", client_cpus or f"{half}-{cpu_count - 1}"


def verbose_flags() -> list[str]:
    """The --verbose a perf binary takes when bb itself runs verbose, else nothing."""
    return ["--verbose"] if log.isEnabledFor(logging.DEBUG) else []
