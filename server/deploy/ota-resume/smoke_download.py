"""Run on the deployment host. Uses one temporary download ticket, never a device identity."""
import hashlib
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
import uuid
from pathlib import Path


def command(args, data=None):
    result = subprocess.run(args, input=data, text=True, capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError("Deployment fixture command failed; credentials omitted")
    return result.stdout.strip()


container = "xiaozhi-esp32-server-redis"
config = json.loads(command(["docker", "inspect", container]))[0]["Config"]
args = config.get("Cmd") or []
password = args[args.index("--requirepass") + 1] if "--requirepass" in args else ""


def redis(*args):
    # Send the password on stdin, not in process arguments or logs.
    script = 'read -r REDISCLI_AUTH; export REDISCLI_AUTH; exec redis-cli --raw "$@"'
    result = command(["docker", "exec", "-i", container, "sh", "-c", script, "sh", *args], password + "\n")
    if result.startswith(("NOAUTH", "WRONGPASS", "ERR")):
        raise RuntimeError("Redis fixture authentication/command failed")
    return result


ticket = str(uuid.uuid4())
key = "ota:id:" + ticket
url = os.environ.get("OTA_SMOKE_BASE", "https://robot.suji.work") + "/xiaozhi/otaMag/download/" + ticket
firmware = Path(os.environ.get("OTA_SMOKE_IMAGE", "/opt/xiaozhi-server/uploadfile/vanex-s3-2.1.3.bin")).read_bytes()
firmware_id = os.environ.get("OTA_SMOKE_ID", "253f7846b98647f6bcdba7429dfe1662")
digest = hashlib.sha256(firmware).hexdigest()
etag = '"' + digest + '"'


def get(headers=None):
    req = urllib.request.Request(url, headers=headers or {})
    try:
        response = urllib.request.urlopen(req, timeout=45)
    except urllib.error.HTTPError as exc:
        response = exc
    with response:
        return response.status, response.headers, response.read()


try:
    assert redis("SET", key, json.dumps(firmware_id), "EX", "600") == "OK"
    if "--probe-before" in sys.argv:
        status, headers, body = get({"Range": "bytes=65536-66559"})
        print("RANGE_PROBE status=%d bytes=%d etag=%s content_range=%s" % (status, len(body), headers.get("ETag"), headers.get("Content-Range")))
    else:
        status, headers, body = get()
        assert status == 200 and body == firmware
        assert int(headers["Content-Length"]) == len(firmware)
        assert headers.get("Content-Encoding", "identity") == "identity"
        status, headers, first = get({"Range": "bytes=0-65535"})
        assert status == 206 and len(first) == 65536
        assert headers["ETag"] == etag and headers["X-Firmware-SHA256"] == digest
        status, headers, rest = get({"Range": "bytes=65536-", "If-Range": etag})
        assert status == 206 and headers["Content-Range"] == "bytes 65536-%d/%d" % (len(firmware) - 1, len(firmware))
        assert hashlib.sha256(first + rest).hexdigest() == digest
        for _ in range(5):
            status, headers, body = get({"Range": "bytes=100-199", "If-Range": etag})
            assert status == 206 and body == firmware[100:200]
        status, headers, body = get({"Range": "bytes=65536-", "If-Range": '"outdated"'})
        assert status == 200 and body == firmware
        status, headers, body = get({"Range": "bytes=%d-" % len(firmware)})
        assert status == 416 and headers["Content-Range"] == "bytes */%d" % len(firmware)
        assert 0 < int(redis("TTL", key)) <= 600
        print("LIVE_OTA_RANGE_OK: 10 requests, legacy 200 full GET, 206 resume, SHA-256 match, >3 requests, 200 restart, 416, TTL preserved")
finally:
    redis("DEL", key, "ota:download:count:" + ticket)
    print("Temporary OTA ticket removed; device records and release versions unchanged")
