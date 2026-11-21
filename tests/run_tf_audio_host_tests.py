"""Exercise the actual TF initialization methods with host files and fake HTTP."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    board = (ROOT / "main/boards/jiuchuan-s3/jiuchuan_dev_board.cc").read_text(encoding="utf-8")
    start = board.index("    enum class TfAudioState")
    end = board.index("    void StartTfAudioInitialization()", start)
    app = (ROOT / "main/application.cc").read_text(encoding="utf-8")
    header = (ROOT / "main/application.h").read_text(encoding="utf-8")
    boot_methods = app[app.index("void Application::NotifyTfStartAudioReady()"):
                       app.index("void Application::PlaySdAudioFile(")]
    queue_types = header[header.index("    struct SdAudioSequenceState"):
                         header.index("    Application();")]
    assert board[start:end].count("Application::GetInstance().NotifyTfStartAudioReady();") == 2
    assert '#ifndef CONFIG_BOARD_TYPE_JIUCHUAN\n    PlaySdAudioFile("SafetyReminder.mp3");\n#endif' in app
    assert 'clock_ticks_++;\n#ifdef CONFIG_BOARD_TYPE_JIUCHUAN\n            TryStartTfStartAudio();' in app
    with tempfile.TemporaryDirectory(prefix="vanex-tf-test-") as folder:
        temp = Path(folder)
        (temp / "tf_audio_production.inc").write_text(board[start:end], encoding="utf-8")
        (temp / "boot_audio_production.inc").write_text(boot_methods, encoding="utf-8")
        (temp / "sd_queue_types.inc").write_text(queue_types, encoding="utf-8")
        for name in ("tf_audio_test", "boot_audio_test"):
            binary = temp / name
            subprocess.run([
                args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + folder, str(ROOT / "tests" / (name + ".cc")), "-o", str(binary)
            ], check=True)
            subprocess.run([str(binary)], cwd=temp, check=True)


if __name__ == "__main__":
    main()
