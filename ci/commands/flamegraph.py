import os
import subprocess

from ci.commands.process import ROOT

FLAMEGRAPH_PL = os.path.join(ROOT, "contrib/FlameGraph/flamegraph.pl")


def render_flamegraph(folded_file: str, out_svg: str, title: str) -> None:
    """Fold silk profiler's stack+on_ns+off_ns lines into stack+ns and render via flamegraph.pl; frame names
    contain spaces, so the two counts are the last two tokens of a line.
    """
    combined_lines: list[str] = []
    with open(folded_file) as handle:
        for line in handle:
            parts = line.strip().rsplit(" ", 2)
            if len(parts) != 3:
                continue
            stack, on_ns, off_ns = parts
            total = int(on_ns) + int(off_ns)
            if total > 0:
                combined_lines.append(f"{stack} {total}\n")

    with open(out_svg, "w") as outfile:
        subprocess.run(
            [FLAMEGRAPH_PL, "--title", title, "--countname=ns", "--hash"],
            input="".join(combined_lines).encode(),
            stdout=outfile,
            cwd=ROOT,
            check=True,
        )
