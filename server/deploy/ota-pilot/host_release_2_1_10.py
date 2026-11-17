"""Advance the confirmed single-device OTA pilot without changing public releases."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid

sys.path.insert(0, "/opt/xiaozhi-server/deploy-20261006-ota-pilot")
from host_pilot import (CAB, MAC, ROOT, database, digest, health, literal,
                        NO_UPDATE_URL, post_check, redis)

DEPLOY = ROOT / "deploy-20261007-ota-2.1.10"
VERSION = "2.1.10"
SHA = "01cb1d8e6411069d7fba048c89273fc8b241c0fa834dfec4c54e062b408e8836"
SIZE = 3208368
NAME = "vanex-s3-2.1.10-01cb1d8e6411.bin"
IMAGE = ROOT / "uploadfile" / NAME
FIXTURES = ("02:fa:07:21:10:01", "02:fa:07:21:10:02")
BASES = ("https://robot.suji.work", "http://47.94.91.32:8002")


def now():
    return datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat()


def load():
    return json.loads((DEPLOY / "state.json").read_text())


def save(state):
    (DEPLOY / "state.json").write_text(json.dumps(state, indent=2) + "\n")


def current_config():
    return database("SELECT param_value FROM sys_params WHERE param_code='server.ota_pilot'")


def set_config(value, expected):
    if value == expected:
        if current_config() != value:
            raise RuntimeError("Pilot changed concurrently")
        return
    changed = database("UPDATE sys_params SET param_value=" + literal(value) + ",update_date=NOW() "
                       "WHERE param_code='server.ota_pilot' AND BINARY param_value=BINARY " +
                       literal(expected) + "; SELECT ROW_COUNT();")
    if changed != "1" or current_config() != value:
        raise RuntimeError("Pilot changed concurrently; no unrelated configuration will be overwritten")


def config_for(state, targets):
    return json.dumps({"enabled": True, "boardType": "jiuchuan-s3", "otaId": state["ota_id"],
                       "macAddresses": targets}, separators=(",", ":"))


def public_rows():
    return database("SELECT id,type,version,size,firmware_path,update_date FROM ai_ota "
                    "WHERE type='jiuchuan-s3' ORDER BY id")


def verify_image(path):
    image = path.read_bytes()
    if (len(image) != SIZE or hashlib.sha256(image).hexdigest() != SHA or image[0] != 0xe9 or
            image[48:80].split(b"\0")[0] != VERSION.encode() or
            int.from_bytes(image[12:14], "little") != 9 or len(image) > 0x3f0000):
        raise RuntimeError("Application image/version/chip/size mismatch")


def stage():
    if (DEPLOY / "state.json").exists():
        raise RuntimeError("This deployment already has state; inspect before retrying")
    verify_image(DEPLOY / "xiaozhi.bin")
    health()
    previous = current_config()
    config = json.loads(previous)
    if (config.get("enabled") is not True or config.get("boardType") != "jiuchuan-s3" or
            config.get("macAddresses") != [MAC]):
        raise RuntimeError("Pilot scope changed; keep the current rollout intact")
    if database("SELECT version FROM ai_ota WHERE id=" + literal(config["otaId"])) != "2.1.9":
        raise RuntimeError("Unexpected previous pilot release")
    if database("SELECT mac_address FROM ai_mqtt_binding_mapping WHERE cab_guid=" + literal(CAB)) != MAC:
        raise RuntimeError("Target mapping changed")
    if database("SELECT COUNT(*) FROM ai_ota WHERE type='pilot-jiuchuan-s3' AND version='2.1.10'") != "0":
        raise RuntimeError("2.1.10 already staged; do not create a duplicate")
    owner = database("SELECT user_id FROM ai_device WHERE mac_address=" + literal(MAC))
    if not owner.isdigit():
        raise RuntimeError("Target owner is ambiguous")
    if IMAGE.exists():
        verify_image(IMAGE)
    else:
        shutil.copy2(DEPLOY / "xiaozhi.bin", IMAGE)
        os.chmod(IMAGE, 0o644)
    state = {"ota_id": uuid.uuid4().hex, "version": VERSION, "sha256": SHA, "size": SIZE,
             "target_cab": CAB, "target_mac": MAC, "previous_config": previous,
             "public_rows_before": public_rows(), "status": "preparing",
             "api_sha256": digest(ROOT / "manager-api/xiaozhi-esp32-api.jar"), "staged_at": now()}
    save(state)
    (DEPLOY / "target-before.tsv").write_text(database(
        "SELECT mac_address,app_version,auto_update,last_connected_at FROM ai_device WHERE mac_address=" + literal(MAC)) + "\n")
    values = [literal(state["ota_id"]), literal("Vanex S3 2.1.10 - single device pilot"),
              literal("pilot-jiuchuan-s3"), literal(VERSION), str(SIZE),
              literal("Target " + CAB + "; immutable SHA256 " + SHA), literal("/uploadfile/" + NAME),
              "0", owner, "NOW()", owner, "NOW()"]
    database("INSERT INTO ai_ota(id,firmware_name,type,version,size,remark,firmware_path,sort,creator,create_date,updater,update_date) "
             "VALUES (" + ",".join(values) + ")")
    state["status"] = "staged-not-selected"
    save(state)
    print("STAGED", VERSION, state["ota_id"], SIZE, SHA, "previous 2.1.9 pilot retained")


def verify():
    state = load()
    verify_image(IMAGE)
    if current_config() != state["previous_config"] or public_rows() != state["public_rows_before"]:
        raise RuntimeError("Rollout changed since staging; inspect before testing")
    for base in BASES:
        env = dict(os.environ, OTA_SMOKE_BASE=base, OTA_SMOKE_IMAGE=str(IMAGE), OTA_SMOKE_ID=state["ota_id"])
        test = subprocess.run([sys.executable, str(DEPLOY / "smoke_download.py")], env=env,
                              capture_output=True, text=True, timeout=600)
        (DEPLOY / ("download-https.log" if base.startswith("https:") else "download-legacy.log")).write_text(test.stdout + test.stderr)
        if test.returncode:
            raise RuntimeError("Download verification failed; inspect protected logs")
        print("DOWNLOAD_OK", base, test.stdout.strip())

    ids = [uuid.uuid4().hex for _ in FIXTURES]
    created = False
    test_config = config_for(state, [FIXTURES[0]])
    switched = False
    tickets = []
    try:
        for mac in FIXTURES:
            if database("SELECT COUNT(*) FROM ai_device WHERE mac_address=" + literal(mac)) != "0":
                raise RuntimeError("Fixture collision; customer rows must not be reused")
        sql = "START TRANSACTION;\n"
        for item, mac in zip(ids, FIXTURES):
            sql += "INSERT INTO ai_device(id,mac_address,board,app_version,auto_update,alias) VALUES (" + ",".join([
                literal(item), literal(mac), literal("jiuchuan-s3"), literal("2.1.0"), "1", literal("temporary 2.1.10 OTA fixture")]) + ");\n"
        database(sql + "COMMIT;")
        created = True
        set_config(test_config, state["previous_config"])
        switched = True
        for base in BASES:
            for old in ("2.1.0", "2.1.9"):
                firmware = post_check(FIXTURES[0], old, base)["firmware"]
                assert firmware["version"] == VERSION
                assert firmware["url"].startswith("https://robot.suji.work/xiaozhi/otaMag/download/")
                ticket = firmware["url"].rsplit("/", 1)[-1]
                tickets.append(ticket)
                assert json.loads(redis("GET", "ota:id:" + ticket)) == state["ota_id"]
            for installed in (VERSION, "2.1.11"):
                assert post_check(FIXTURES[0], installed, base)["firmware"]["url"] == NO_UPDATE_URL
            other = post_check(FIXTURES[1], "2.1.0", base)["firmware"]
            assert other["version"] == "2.1.3"
            assert other["url"].startswith("https://robot.suji.work/xiaozhi/otaMag/download/")
            tickets.append(other["url"].rsplit("/", 1)[-1])
            assert "firmware" not in post_check(FIXTURES[1], "2.1.0", base, "pilot-jiuchuan-s3")
            database("UPDATE ai_device SET auto_update=0 WHERE id=" + literal(ids[0]))
            assert "firmware" not in post_check(FIXTURES[0], "2.1.0", base)
            database("UPDATE ai_device SET auto_update=1 WHERE id=" + literal(ids[0]))
            print("SELECTION_OK", base, "2.1.0/2.1.9 -> 2.1.10; same/newer unchanged; other -> 2.1.3; opt-out respected")
    finally:
        try:
            if switched:
                set_config(state["previous_config"], test_config)
        finally:
            if created:
                time.sleep(1)
                for item, mac in zip(ids, FIXTURES):
                    database("DELETE FROM ai_device WHERE id=" + literal(item) + " AND mac_address=" + literal(mac))
            for ticket in tickets:
                redis("DEL", "ota:id:" + ticket, "ota:download:count:" + ticket)
    if public_rows() != state["public_rows_before"]:
        raise RuntimeError("Public release changed during verification")
    health()
    state["verified_api_sha256"] = digest(ROOT / "manager-api/xiaozhi-esp32-api.jar")
    state["verified"] = True
    state["verified_at"] = now()
    save(state)
    print("VERIFIED; temporary fixtures/tickets removed; actual device telemetry not simulated")


def activate():
    state = load()
    if not state.get("verified") or state.get("verified_api_sha256") != digest(ROOT / "manager-api/xiaozhi-esp32-api.jar"):
        raise RuntimeError("Current deployment must pass verification before activation")
    verify_image(IMAGE)
    if public_rows() != state["public_rows_before"]:
        raise RuntimeError("Public releases changed since staging")
    if database("SELECT auto_update FROM ai_device WHERE mac_address=" + literal(MAC)) != "1":
        raise RuntimeError("Respect target auto-update opt-out")
    set_config(config_for(state, [MAC]), state["previous_config"])
    state["status"] = "active-for-one-device"
    state["activated_at"] = now()
    save(state)
    print("PILOT_ACTIVE", CAB, MAC, VERSION)
    print("No container restart, forced device restart or fleet release")


def rollback():
    state = load()
    set_config(state["previous_config"], config_for(state, [MAC]))
    state["status"] = "rolled-back-to-previous-pilot"
    save(state)
    print("Previous pilot selection restored; no device downgraded")


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["stage", "verify", "activate", "rollback"])
    action = parser.parse_args().action
    {"stage": stage, "verify": verify, "activate": activate, "rollback": rollback}[action]()
