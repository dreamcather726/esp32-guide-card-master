"""Run production audio lifecycle/codec methods against a threaded host RTOS."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
    header = (ROOT / "main/audio/audio_service.h").read_text(encoding="utf-8")
    sections = [
        ("void AudioService::Start()", "bool AudioService::ReadAudioData("),
        ("void AudioService::OpusCodecTask()", "std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue()"),
        ("void AudioService::ResetDecoder()", "void AudioService::CheckAndUpdateAudioPowerState()"),
    ]
    with tempfile.TemporaryDirectory(prefix="vanex-audio-runtime-") as folder:
        temp = Path(folder)
        (temp / "audio_constants.inc").write_text(
            header[header.index("#define OPUS_FRAME_DURATION_MS"):header.index("#define AS_OPUS_GET_FRAME_DRU_ENUM")],
            encoding="utf-8",
        )
        (temp / "audio_runtime.inc").write_text(
            "\n".join(source[source.index(start):source.index(end)] for start, end in sections),
            encoding="utf-8",
        )
        binary = temp / "audio_runtime_test"
        subprocess.run([
            args.cxx, "-std=c++17", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + folder,
            str(ROOT / "tests/audio_runtime_test.cc"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
