"""Run the production TF QR worker and display methods with deterministic host I/O."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "main/boards/jiuchuan-s3/jiuchuan_dev_board.cc").read_text(encoding="utf-8")
    worker = source[source.index("    enum class TfQrState"):
                    source.index("    void StartTfQrInitialization()")]
    with tempfile.TemporaryDirectory(prefix="vanex-qr-test-") as folder:
        temp = Path(folder)
        (temp / "tf_qr_production.inc").write_text(worker, encoding="utf-8")
        for name in ("startup_flow_test", "startup_qr_jpeg_test", "tf_qr_test"):
            binary = temp / name
            subprocess.run([
                args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + folder, str(ROOT / "tests" / (name + ".cc")), "-o", str(binary)
            ], check=True)
            subprocess.run([str(binary)], cwd=temp, check=True)


if __name__ == "__main__":
    main()
