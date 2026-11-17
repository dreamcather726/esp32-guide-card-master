# TF Start Audio Initialization (2.1.9)

## Paths

- Source: `https://iotossbj.suji.work/SDfile/TF/start.wav`
- Device filesystem: `/sdcard/start.wav` (TF card root, not a `TF` subdirectory).
- Staging file: `/sdcard/.start.wav.part`.

## Behavior

Earlier firmware only downloaded when the entire TF root was empty. Any role
folder, OS-created directory, or staging file left by a power loss skipped the
download, even if `start.wav` was absent.

On each boot, 2.1.9 checks the target file independently of other card contents.
A missing, empty, invalid-header, or truncated RIFF/WAVE file is downloaded again.
An existing file that passes the header/length check is preserved without a
network request. This check is not a cryptographic content-integrity check.

Downloads run in a background worker. USB network readiness is awaited before
HTTP, and transient HTTP failures retry with 2/4/8/16/32/60-second backoff, capped
at 60 seconds. WiFi retries do not depend on USB readiness. No reboot or formatting
is needed just because the network was late.

The worker checks writes, flush, fsync, close, HTTP length (when known), and the
staged WAV header/RIFF length before replacing the invalid target. Interrupted
downloads are retried from the beginning; the file is small. TF read/write/path
errors stop this worker and display the existing internal-storage error message.
Fix the card and reboot to retry. Other files and directories are never cleared.

This change ensures the file is present; it does not change when audio is played,
binding, OTA delivery, role audio paths, or server configuration.

## Logs

- `[TF_INIT] path=/sdcard/start.wav needs download: ...`
- `[TF_INIT] waiting for USB network; download pending`
- `[TF_INIT] GET https://.../start.wav -> /sdcard/start.wav`
- `[TF_INIT][RETRY] ...; retry in ... ms`
- `[TF_INIT][READY] path=/sdcard/start.wav bytes=... downloaded and verified`
- `[TF_INIT][READY] path=/sdcard/start.wav already valid; skip download`
- `[TF_INIT][ERROR] ...` for storage failures.

## Host Regression

On a POSIX host with Python 3 and a C++17 compiler:

```sh
python3 tests/run_tf_audio_host_tests.py --cxx g++
```

The test runner compiles the production worker methods verbatim, with a fake HTTP
client and temporary host files. It covers populated/empty cards, leftovers after
power loss, valid-file preservation, invalid/truncated files, delayed network,
long outages/backoff, partial HTTP responses, write-path errors, and fsync failure.
Physical SDMMC, TLS on the modem, and power loss during flash/card writes still
require a real-device test.
