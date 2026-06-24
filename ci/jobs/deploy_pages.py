import shutil
import tarfile
from pathlib import Path

from praktika.gh import GH
from praktika.info import Info
from praktika.result import Result
from praktika.settings import Settings


COVERAGE_HTML_ARCHIVE = Path(Settings.INPUT_DIR) / "coverage-html.tar.gz"
COVERAGE_HTML_DIR = Path(Settings.TEMP_DIR) / "coverage/html_publish"


def _extract_coverage_archive():
    if not COVERAGE_HTML_ARCHIVE.is_file():
        raise FileNotFoundError(f"Coverage archive not found: {COVERAGE_HTML_ARCHIVE}")

    if COVERAGE_HTML_DIR.exists():
        shutil.rmtree(COVERAGE_HTML_DIR)
    COVERAGE_HTML_DIR.mkdir(parents=True, exist_ok=True)

    with tarfile.open(COVERAGE_HTML_ARCHIVE, "r:gz") as tar:
        target_root = COVERAGE_HTML_DIR.resolve()
        for member in tar.getmembers():
            target = (COVERAGE_HTML_DIR / member.name).resolve()
            if target_root not in (target, *target.parents):
                raise RuntimeError(f"Invalid coverage archive path: {member.name}")
        try:
            tar.extractall(COVERAGE_HTML_DIR, filter="data")
        except TypeError:
            tar.extractall(COVERAGE_HTML_DIR)

    if not (COVERAGE_HTML_DIR / "index.html").is_file():
        raise RuntimeError(
            f"Coverage archive did not extract an index.html into {COVERAGE_HTML_DIR}"
        )


def main():
    info = Info()

    _extract_coverage_archive()
    url = GH.publish_gh_pages(
        str(COVERAGE_HTML_DIR),
        destination_dir="coverage",
        commit_message=f"Publish coverage report for {info.sha[:12]}",
    )
    result = Result.create_from(
        name="Publish Coverage Report",
        status=Result.Status.OK,
        info=f"Coverage report: {url}",
        links=[url],
    )
    result.set_label("coverage", link=url)
    result.complete_job()


if __name__ == "__main__":
    main()
