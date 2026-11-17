"""Stage and verify the diagnostic application without selecting any device."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid

sys.path.insert(0, "/opt/xiaozhi-server/deploy-20261006-ota-pilot")
from host_pilot import ROOT, database, digest, health, literal, redis

DEPLOY = ROOT / "deploy-20261021-ota-2.1.11-diag"
VERSION = "2.1.11-diag"
SHA = "6b1926c928740b04df0794d9e5ee7f8dc518e9afa56fdc1219ad7f23fe266170"
SIZE = 3231984
ELF_SHA = "25a93c8175eb1bac82829081944b846200baa0c2de6d2366cc5a92ddd6608f19"
NAME = "vanex-s3-2.1.11-diag-6b1926c92874.bin"
IMAGE = ROOT / "uploadfile" / NAME
TYPE = "pilot-jiuchuan-s3"
BASES = ("https://robot.suji.work", "http://47.94.91.32:8002")


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def snapshot():
    return {
        "public_rows": database("SELECT id,type,version,size,firmware_path,update_date FROM ai_ota "
                                "WHERE type NOT LIKE 'pilot-%' ORDER BY id"),
        "pilot_config": database("SELECT param_value FROM sys_params WHERE param_code='server.ota_pilot'"),
        "api_sha256": digest(ROOT / "manager-api/xiaozhi-esp32-api.jar"),
    }


def save(state):
    temporary = DEPLOY / "state.json.tmp"
    temporary.write_text(json.dumps(state, indent=2) + "\n")
    temporary.replace(DEPLOY / "state.json")


def load():
    return json.loads((DEPLOY / "state.json").read_text())


def verify_image(path):
    image = path.read_bytes()
    if (len(image) != SIZE or hashlib.sha256(image).hexdigest() != SHA or image[0] != 0xe9 or
            image[48:80].split(b"\0")[0] != VERSION.encode() or
            image[0xb0:0xd0].hex() != ELF_SHA or
            int.from_bytes(image[12:14], "little") != 9 or len(image) > 0x3f0000):
        raise RuntimeError("Diagnostic application identity mismatch")


def assert_unchanged(state):
    if snapshot() != state["before"]:
        raise RuntimeError("Production release/configuration changed; inspect before continuing")
    expected = "\t".join((TYPE, VERSION, str(SIZE), "/uploadfile/" + NAME))
    actual = database("SELECT type,version,size,firmware_path FROM ai_ota WHERE id=" + literal(state["ota_id"]))
    if actual != expected:
        raise RuntimeError("Staged OTA metadata mismatch")
    verify_image(IMAGE)


def stage():
    verify_image(DEPLOY / "xiaozhi.bin")
    health()
    if (DEPLOY / "state.json").exists():
        state = load()
        if snapshot() != state["before"]:
            raise RuntimeError("Existing staging baseline changed")
    else:
        if database("SELECT COUNT(*) FROM ai_ota WHERE type=" + literal(TYPE) +
                    " AND version=" + literal(VERSION)) != "0":
            raise RuntimeError("Diagnostic version already exists; inspect before retrying")
        state = {"ota_id": uuid.uuid4().hex, "version": VERSION, "sha256": SHA, "size": SIZE,
                 "status": "preparing", "before": snapshot(), "staged_at": now()}
        save(state)

    if IMAGE.exists():
        verify_image(IMAGE)
    else:
        with tempfile.NamedTemporaryFile(dir=IMAGE.parent, prefix=".diag-upload-", delete=False) as output:
            temporary = Path(output.name)
            with (DEPLOY / "xiaozhi.bin").open("rb") as source:
                shutil.copyfileobj(source, output)
            output.flush()
            os.fsync(output.fileno())
        try:
            verify_image(temporary)
            os.chmod(temporary, 0o644)
            os.link(temporary, IMAGE)
        finally:
            temporary.unlink()

    if database("SELECT COUNT(*) FROM ai_ota WHERE id=" + literal(state["ota_id"])) == "0":
        owner = database("SELECT creator FROM ai_ota WHERE type='jiuchuan-s3' AND version='2.1.10' "
                         "ORDER BY update_date DESC LIMIT 1")
        if not owner.isdigit():
            raise RuntimeError("Release owner is not available")
        values = [literal(state["ota_id"]), literal("Vanex S3 " + VERSION + " - staged only"), literal(TYPE),
                  literal(VERSION), str(SIZE), literal("Not selected for devices; diagnostic image SHA256 " + SHA),
                  literal("/uploadfile/" + NAME), "0", owner, "NOW()", owner, "NOW()"]
        database("INSERT INTO ai_ota(id,firmware_name,type,version,size,remark,firmware_path,sort,"
                 "creator,create_date,updater,update_date) VALUES (" + ",".join(values) + ")")
    assert_unchanged(state)
    state["status"] = "staged-not-selected"
    save(state)
    print("STAGED_NOT_SELECTED", state["ota_id"], VERSION, SIZE, SHA)


def verify():
    state = load()
    assert_unchanged(state)
    for index, base in enumerate(BASES):
        env = dict(os.environ, OTA_SMOKE_BASE=base, OTA_SMOKE_IMAGE=str(IMAGE), OTA_SMOKE_ID=state["ota_id"])
        result = subprocess.run([sys.executable, str(DEPLOY / "smoke_download.py")], env=env,
                                capture_output=True, text=True, timeout=600)
        (DEPLOY / ("download-%d.log" % index)).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError("OTA download validation failed; see protected deployment log")
        print(base, result.stdout.strip())
    assert_unchanged(state)
    health()
    state.update(verified_at=now(), verified=True)
    save(state)


def download_url():
    state = load()
    assert_unchanged(state)
    if not state.get("verified"):
        raise RuntimeError("Verify the uploaded image before issuing a link")
    ticket = str(uuid.uuid4())
    if redis("SET", "ota:id:" + ticket, json.dumps(state["ota_id"]), "EX", "86400", "NX") != "OK":
        raise RuntimeError("Download ticket was not created")
    expires = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(seconds=86400)
    info = {
        "version": VERSION, "size": SIZE, "sha256": SHA, "otaId": state["ota_id"],
        "url": BASES[0] + "/xiaozhi/otaMag/download/" + ticket,
        "expiresAt": expires.isoformat(), "serverFile": str(IMAGE),
        "deviceSelectionChanged": False, "fleetVersion": "2.1.10",
    }
    (DEPLOY / "download-link.json").write_text(json.dumps(info, indent=2) + "\n")
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["inspect", "stage", "verify", "download-url"])
    action = parser.parse_args().action
    with (DEPLOY / ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if action == "inspect":
            print(json.dumps(snapshot(), indent=2))
        else:
            {"stage": stage, "verify": verify, "download-url": download_url}[action]()
