import os
import signal
import subprocess

from ci.commands.cmake import cmd_build
from ci.commands.flamegraph import render_flamegraph
from ci.commands.process import ROOT, log, start_process, verbose_flags


def _start_profiler(preset: str, pid: int, folded_stacks: str) -> subprocess.Popen[str]:
    """Start silk's BPF profiler sampling pid on-CPU and off-CPU with kernel stacks into folded_stacks."""
    profiler_bin = os.path.join(ROOT, f"build/{preset}/bin/profiler")
    with open(folded_stacks, "w") as handle:
        return start_process(
            profiler_bin,
            "--pid",
            str(pid),
            "--on-cpu",
            "--off-cpu",
            "--kernel-stacks",
            *verbose_flags(),
            stdout=handle,
        )


def run_under_profiler(
    preset: str, target: str, client_args: list[str], server_pid: int | None = None
) -> None:
    """Run the client command line under silk's BPF profiler and render an on-CPU + off-CPU flamegraph as
    <target>.flamegraph.svg; with server_pid, profile that server alongside into <target>-server.
    """
    cmd_build(preset, ["profiler"])

    folded_stacks = os.path.join(ROOT, f"build/{preset}/{target}.flamegraph.folded")
    out_svg = os.path.join(ROOT, f"build/{preset}/{target}.flamegraph.svg")
    server_folded = os.path.join(
        ROOT, f"build/{preset}/{target}-server.flamegraph.folded"
    )
    server_svg = os.path.join(ROOT, f"build/{preset}/{target}-server.flamegraph.svg")

    log.info("profiling %s -> %s", target, out_svg)

    client = start_process(*client_args, stdout=subprocess.DEVNULL)
    profilers: list[subprocess.Popen[str]] = []
    try:
        profilers.append(_start_profiler(preset, client.pid, folded_stacks))
        if server_pid is not None:
            profilers.append(_start_profiler(preset, server_pid, server_folded))
        client.wait()
    finally:
        if client.poll() is None:
            client.kill()
            client.wait()
        for profiler in profilers:
            profiler.send_signal(signal.SIGTERM)
            profiler.wait()
    if client.returncode != 0:
        raise subprocess.CalledProcessError(client.returncode, client_args)
    for profiler in profilers:
        if profiler.returncode != 0:
            raise subprocess.CalledProcessError(profiler.returncode, profiler.args)

    render_flamegraph(folded_stacks, out_svg, f"{target} on-CPU + off-CPU")
    if server_pid is not None:
        render_flamegraph(
            server_folded, server_svg, f"{target} server on-CPU + off-CPU"
        )
        log.info("server folded stacks: %s", server_folded)

    log.info("folded stacks: %s", folded_stacks)
    log.info("flamegraph:    %s", out_svg)
