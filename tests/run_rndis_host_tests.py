"""Compile host checks using the production worker methods and simulated I/O."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    board = (ROOT / "main/boards/common/rndis_board.cc").read_text(encoding="utf-8")
    # These exact, contiguous definitions are exercised without copying their logic.
    sections = [
        ("bool RndisBoard::DetectAndDialUsbAt()", "void RndisBoard::NetworkTaskEntry("),
        ("void RndisBoard::NetworkTask()", "static constexpr const char* kPrimaryDnsServer"),
    ]
    with tempfile.TemporaryDirectory(prefix="vanex-rndis-test-") as folder:
        temp = Path(folder)
        implementations = [board[board.index(start):board.index(end)] for start, end in sections]
        (temp / "rndis_production.inc").write_text("\n".join(implementations), encoding="utf-8")
        for name in ("ml307_at_test", "rndis_board_flow_test"):
            binary = temp / (name + ".exe")
            subprocess.run([
                args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-I" + str(ROOT / "main/boards/common"), "-I" + folder,
                str(ROOT / "tests" / (name + ".cc")), "-o", str(binary)
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
