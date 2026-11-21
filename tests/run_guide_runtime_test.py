"""Run real Application queue and display adapter methods with fake RTOS and I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "main/application.cc").read_text(encoding="utf-8")
header = (ROOT / "main/application.h").read_text(encoding="utf-8")
sections = [
    ("void Application::FinishTtsWhenIdle()", "void Application::HandleNetworkConnectedEvent()"),
    ("void Application::PlaySdAudioSequence(", "void Application::InterruptCurrentPlayback()"),
    ("void Application::CancelSdAudioPlaybackOnMainTask()", "uint64_t Application::GetSdAudioPlaybackDurationSeconds()"),
    ("void Application::StartNextSdAudioPlayback()", "void Application::HandleToggleChatEvent()"),
]
with tempfile.TemporaryDirectory(prefix="jiuchuan-guide-runtime-") as folder:
    temp = Path(folder)
    (temp / "guide_runtime.inc").write_text("\n".join(source[source.index(start):source.index(end)] for start, end in sections))
    (temp / "queue_types.inc").write_text(header[header.index("    struct SdAudioSequenceState"):header.index("    Application();")])
    binary = temp / "guide_runtime_test"
    subprocess.run(["g++", "-std=c++17", "-g", "-Wall", "-Wextra", "-Werror", "-no-pie",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + folder,
                    str(ROOT / "tests/guide_runtime_test.cc"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], cwd=temp, check=True)
