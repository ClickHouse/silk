import glob
import os
import sys
import time
import xml.etree.ElementTree as ET
from typing import TypedDict

from ci.commands.process import ROOT, log, run


class _FileInfo(TypedDict):
    lines: dict[int, int]
    branches: dict[int, list[int]]


def _lcov_to_cobertura(lcov_path: str, xml_path: str) -> None:
    files: dict[str, _FileInfo] = {}
    current: str | None = None

    with open(lcov_path) as handle:
        for raw in handle:
            line = raw.rstrip()
            if line.startswith("SF:"):
                current = line[3:]
                files[current] = {"lines": {}, "branches": {}}
            elif line.startswith("DA:") and current:
                parts = line[3:].split(",")
                files[current]["lines"][int(parts[0])] = int(parts[1])
            elif line.startswith("BRDA:") and current:
                parts = line[5:].split(",")
                lineno, taken = int(parts[0]), parts[3]
                count = 0 if taken == "-" else int(taken)
                files[current]["branches"].setdefault(lineno, []).append(count)
            elif line == "end_of_record":
                current = None

    total_lines_valid = total_lines_hit = total_branches_valid = total_branches_hit = 0

    root = ET.Element("coverage")
    ET.SubElement(ET.SubElement(root, "sources"), "source").text = ROOT
    pkgs_el = ET.SubElement(root, "packages")

    by_pkg: dict[str, list[str]] = {}
    for fname in sorted(files):
        by_pkg.setdefault(os.path.dirname(fname) or ".", []).append(fname)

    for pkg_name, fnames in sorted(by_pkg.items()):
        pkg_el = ET.SubElement(
            pkgs_el, "package", name=pkg_name.replace("/", "."), complexity="0"
        )
        classes_el = ET.SubElement(pkg_el, "classes")
        pkg_lines_valid = pkg_lines_hit = pkg_branches_valid = pkg_branches_hit = 0

        for fname in sorted(fnames):
            data = files[fname]
            class_el = ET.SubElement(
                classes_el,
                "class",
                name=os.path.basename(fname),
                filename=fname,
                complexity="0",
            )
            ET.SubElement(class_el, "methods")
            lines_el = ET.SubElement(class_el, "lines")
            file_lines_valid = file_lines_hit = 0
            file_branches_valid = file_branches_hit = 0

            for lineno in sorted(data["lines"]):
                hits = data["lines"][lineno]
                attrs: dict[str, str] = {"number": str(lineno), "hits": str(hits)}
                branches = data["branches"].get(lineno, [])
                if branches:
                    branches_hit = sum(1 for taken in branches if taken > 0)
                    branches_total = len(branches)
                    pct = (
                        round(100 * branches_hit / branches_total)
                        if branches_total
                        else 0
                    )
                    attrs["branch"] = "true"
                    attrs["condition-coverage"] = (
                        f"{pct}% ({branches_hit}/{branches_total})"
                    )
                    file_branches_valid += branches_total
                    file_branches_hit += branches_hit
                else:
                    attrs["branch"] = "false"
                ET.SubElement(lines_el, "line", attrs)
                file_lines_valid += 1
                if hits > 0:
                    file_lines_hit += 1

            line_rate = file_lines_hit / file_lines_valid if file_lines_valid else 0.0
            branch_rate = (
                file_branches_hit / file_branches_valid if file_branches_valid else 0.0
            )
            class_el.set("line-rate", f"{line_rate:.4f}")
            class_el.set("branch-rate", f"{branch_rate:.4f}")
            pkg_lines_valid += file_lines_valid
            pkg_lines_hit += file_lines_hit
            pkg_branches_valid += file_branches_valid
            pkg_branches_hit += file_branches_hit

        pkg_line_rate = pkg_lines_hit / pkg_lines_valid if pkg_lines_valid else 0.0
        pkg_branch_rate = (
            pkg_branches_hit / pkg_branches_valid if pkg_branches_valid else 0.0
        )
        pkg_el.set("line-rate", f"{pkg_line_rate:.4f}")
        pkg_el.set("branch-rate", f"{pkg_branch_rate:.4f}")
        total_lines_valid += pkg_lines_valid
        total_lines_hit += pkg_lines_hit
        total_branches_valid += pkg_branches_valid
        total_branches_hit += pkg_branches_hit

    root.set("version", "5.0")
    root.set("timestamp", str(int(time.time())))
    root.set("lines-valid", str(total_lines_valid))
    root.set("lines-covered", str(total_lines_hit))
    root.set(
        "line-rate",
        f"{total_lines_hit / total_lines_valid:.4f}" if total_lines_valid else "0.0000",
    )
    root.set("branches-valid", str(total_branches_valid))
    root.set("branches-covered", str(total_branches_hit))
    root.set(
        "branch-rate",
        (
            f"{total_branches_hit / total_branches_valid:.4f}"
            if total_branches_valid
            else "0.0000"
        ),
    )
    root.set("complexity", "0")

    tree = ET.ElementTree(root)
    ET.indent(tree, space="  ")
    with open(xml_path, "w") as handle:
        handle.write('<?xml version="1.0" ?>\n')
        tree.write(handle, encoding="unicode", xml_declaration=False)


def gen_coverage_report(preset: str, profiles_dir: str) -> None:
    build_dir = os.path.join(ROOT, f"build/{preset}")
    profdata_path = os.path.join(build_dir, "coverage.profdata")
    report_dir = os.path.join(build_dir, "html")
    bin_dir = os.path.join(build_dir, "bin")

    profraw_files = glob.glob(os.path.join(profiles_dir, "*.profraw"))
    if not profraw_files:
        log.error("no .profraw files found in %s", profiles_dir)
        sys.exit(1)

    run("llvm-profdata-21", "merge", "-sparse", *profraw_files, "-o", profdata_path)

    test_bins = sorted(glob.glob(os.path.join(bin_dir, "*-test")))
    if not test_bins:
        log.error("no *-test binaries found in %s", bin_dir)
        sys.exit(1)

    common_cov_args = [test_bins[0]]
    for binary in test_bins[1:]:
        common_cov_args += ["-object", binary]
    common_cov_args += [
        f"-instr-profile={profdata_path}",
        "-ignore-filename-regex=/usr/|/_deps/|/libbacktrace-generated/|contrib/|-test\\.cpp",
        "-Xdemangler=c++filt",
    ]

    run(
        "llvm-cov-21",
        "show",
        *common_cov_args,
        "-format=html",
        f"-output-dir={report_dir}",
        "-show-line-counts-or-regions",
    )

    lcov_path = os.path.join(build_dir, "coverage.lcov")
    with open(lcov_path, "w") as lcov_file:
        run(
            "llvm-cov-21",
            "export",
            *common_cov_args,
            "--format=lcov",
            stdout=lcov_file,
        )

    xml_path = os.path.join(build_dir, "coverage.xml")
    _lcov_to_cobertura(lcov_path, xml_path)

    log.info("coverage report: %s/index.html", report_dir)
    log.info("coverage lcov:   %s", lcov_path)
    log.info("coverage xml:    %s", xml_path)
