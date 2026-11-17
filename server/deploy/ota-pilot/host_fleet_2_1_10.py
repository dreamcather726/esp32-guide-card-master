"""Publish the immutable 2.1.10 app to all jiuchuan-s3 devices, preserving opt-outs."""
import argparse
import json
import os
import subprocess
import sys
import time
import uuid

sys.path.insert(0, "/opt/xiaozhi-server/deploy-20261007-ota-2.1.10")
from host_release_2_1_10 import (BASES, IMAGE, NAME, ROOT, SHA, SIZE, VERSION,
    NO_UPDATE_URL, current_config, database, digest, health, literal, now,
    post_check, public_rows, redis, verify_image)

DEPLOY = ROOT / "deploy-20261007-ota-2.1.10-fleet"
FIXTURES = ("02:fa:07:21:20:01", "02:fa:07:21:20:02")
DISABLED_PILOT = '{"enabled":false}'


def load():
    return json.loads((DEPLOY / "state.json").read_text())


def save(state):
    (DEPLOY / "state.json").write_text(json.dumps(state, indent=2) + "\n")


def latest_id():
    return database("SELECT id FROM ai_ota WHERE type='jiuchuan-s3' ORDER BY update_date DESC LIMIT 1")


def download_smoke(ota_id, phase):
    for base in BASES:
        env = dict(os.environ, OTA_SMOKE_BASE=base, OTA_SMOKE_IMAGE=str(IMAGE), OTA_SMOKE_ID=ota_id)
        result = subprocess.run([sys.executable, str(DEPLOY / "smoke_download.py")], env=env,
                                capture_output=True, text=True, timeout=600)
        suffix = "https" if base.startswith("https:") else "legacy"
        (DEPLOY / (phase + "-" + suffix + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError("Download verification failed; inspect protected deployment log")
        print("DOWNLOAD_OK", phase, base)


def prepare():
    if (DEPLOY / "state.json").exists():
        raise RuntimeError("Deployment state exists; inspect before retrying")
    health()
    verify_image(IMAGE)
    previous = current_config()
    config = json.loads(previous)
    if config.get("enabled") is not True or config.get("boardType") != "jiuchuan-s3":
        raise RuntimeError("Unexpected previous pilot policy")
    pilot_id = config["otaId"]
    expected = "\t".join(["pilot-jiuchuan-s3", VERSION, str(SIZE), "/uploadfile/" + NAME])
    if database("SELECT type,version,size,firmware_path FROM ai_ota WHERE id=" + literal(pilot_id)) != expected:
        raise RuntimeError("Current pilot is not the verified immutable 2.1.10 app")
    if database("SELECT COUNT(*) FROM ai_ota WHERE type='jiuchuan-s3' AND version='2.1.10'") != "0":
        raise RuntimeError("Public 2.1.10 already exists; do not duplicate")
    owner = database("SELECT creator FROM ai_ota WHERE id=" + literal(pilot_id))
    if not owner.isdigit():
        raise RuntimeError("Release owner is ambiguous")
    state = {"ota_id": uuid.uuid4().hex, "version": VERSION, "sha256": SHA, "size": SIZE,
             "owner": owner, "previous_config": previous, "previous_latest_id": latest_id(),
             "public_rows_before": public_rows(), "pilot_id": pilot_id,
             "api_sha256": digest(ROOT / "manager-api/xiaozhi-esp32-api.jar"),
             "status": "preparing", "prepared_at": now()}
    save(state)
    (DEPLOY / "devices-before.tsv").write_text(database(
        "SELECT id,mac_address,board,app_version,auto_update,last_connected_at FROM ai_device ORDER BY id") + "\n")
    download_smoke(pilot_id, "preflight")
    state["status"] = "ready-to-publish"
    save(state)
    print("PREPARED; no public release or device settings changed")


def publish():
    state = load()
    if state["status"] != "ready-to-publish":
        raise RuntimeError("Preflight must complete before publishing")
    health()
    verify_image(IMAGE)
    if current_config() != state["previous_config"] or public_rows() != state["public_rows_before"]:
        raise RuntimeError("Release configuration changed since preflight")
    if digest(ROOT / "manager-api/xiaozhi-esp32-api.jar") != state["api_sha256"]:
        raise RuntimeError("API changed since preflight")
    values = [literal(state["ota_id"]), literal("Vanex S3 2.1.10 - all compatible devices"),
              literal("jiuchuan-s3"), literal(VERSION), str(SIZE),
              literal("User-approved fleet release; SHA256 " + SHA), literal("/uploadfile/" + NAME),
              "0", state["owner"], "NOW()", state["owner"], "NOW()"]
    sql = "START TRANSACTION; SET @allowed=0;\n"
    sql += "SELECT (BINARY param_value=BINARY " + literal(state["previous_config"]) + ") INTO @allowed "
    sql += "FROM sys_params WHERE param_code='server.ota_pilot' FOR UPDATE;\n"
    sql += "INSERT INTO ai_ota(id,firmware_name,type,version,size,remark,firmware_path,sort,creator,create_date,updater,update_date) "
    sql += "SELECT " + ",".join(values) + " WHERE @allowed=1 AND NOT EXISTS "
    sql += "(SELECT 1 FROM ai_ota WHERE type='jiuchuan-s3' AND version='2.1.10');\n"
    sql += "SET @inserted=ROW_COUNT();\n"
    sql += "UPDATE sys_params SET param_value=" + literal(DISABLED_PILOT) + ",update_date=NOW() "
    sql += "WHERE param_code='server.ota_pilot' AND @inserted=1; COMMIT; SELECT @inserted;"
    if database(sql) != "1":
        raise RuntimeError("Concurrent release/policy change; public insertion was not applied")
    state["status"] = "published-awaiting-live-checks"
    state["published_at"] = now()
    save(state)
    if latest_id() != state["ota_id"] or current_config() != DISABLED_PILOT:
        raise RuntimeError("Publication audit mismatch")
    print("FLEET_PUBLISHED", VERSION, "jiuchuan-s3", state["ota_id"], state["published_at"])
    print("Single-device policy disabled; opt-out switches preserved; no forced reboots")


def selection_smoke(state):
    ids = [uuid.uuid4().hex for _ in FIXTURES]
    created = False
    tickets = []
    try:
        for mac in FIXTURES:
            if database("SELECT COUNT(*) FROM ai_device WHERE mac_address=" + literal(mac)) != "0":
                raise RuntimeError("Fixture MAC collision")
        sql = "START TRANSACTION;\n"
        for item, mac in zip(ids, FIXTURES):
            sql += "INSERT INTO ai_device(id,mac_address,board,app_version,auto_update,alias) VALUES (" + ",".join([
                literal(item), literal(mac), literal("jiuchuan-s3"), literal("2.1.0"), "1",
                literal("temporary fleet OTA verification")]) + ");\n"
        database(sql + "COMMIT;")
        created = True
        for base in BASES:
            for mac in FIXTURES:
                for old in ("2.1.0", "2.1.2", "2.1.3", "2.1.9"):
                    firmware = post_check(mac, old, base)["firmware"]
                    assert firmware["version"] == VERSION
                    assert firmware["url"].startswith("https://robot.suji.work/xiaozhi/otaMag/download/")
                    ticket = firmware["url"].rsplit("/", 1)[-1]
                    tickets.append(ticket)
                    assert json.loads(redis("GET", "ota:id:" + ticket)) == state["ota_id"]
            for installed in (VERSION, "2.1.11"):
                assert post_check(FIXTURES[0], installed, base)["firmware"]["url"] == NO_UPDATE_URL
            database("UPDATE ai_device SET auto_update=0 WHERE id=" + literal(ids[0]))
            assert "firmware" not in post_check(FIXTURES[0], "2.1.0", base)
            database("UPDATE ai_device SET auto_update=1 WHERE id=" + literal(ids[0]))
            assert post_check(FIXTURES[1], "2.1.0", base, "unrelated-board")["firmware"]["url"] == NO_UPDATE_URL
            assert "firmware" not in post_check(FIXTURES[1], "2.1.0", base, "pilot-jiuchuan-s3")
            print("FLEET_SELECTION_OK", base, "13 checks; unlisted MACs -> 2.1.10; opt-out and board isolation preserved")
    finally:
        if created:
            time.sleep(1)
            for item, mac in zip(ids, FIXTURES):
                database("DELETE FROM ai_device WHERE id=" + literal(item) + " AND mac_address=" + literal(mac))
        for ticket in tickets:
            redis("DEL", "ota:id:" + ticket, "ota:download:count:" + ticket)


def rollback():
    state = load()
    if latest_id() != state["ota_id"] or current_config() != DISABLED_PILOT:
        raise RuntimeError("A later release/policy changed; do not overwrite it")
    sql = "START TRANSACTION; SET @allowed=0;\n"
    sql += "SELECT (BINARY param_value=BINARY " + literal(DISABLED_PILOT) + ") INTO @allowed "
    sql += "FROM sys_params WHERE param_code='server.ota_pilot' FOR UPDATE;\n"
    sql += "UPDATE ai_ota SET type='pilot-withdrawn-jiuchuan-s3',update_date=NOW() WHERE id=" + literal(state["ota_id"])
    sql += " AND type='jiuchuan-s3' AND @allowed=1; SET @withdrawn=ROW_COUNT();\n"
    sql += "UPDATE sys_params SET param_value=" + literal(state["previous_config"]) + ",update_date=NOW() "
    sql += "WHERE param_code='server.ota_pilot' AND @withdrawn=1; COMMIT; SELECT @withdrawn;"
    if database(sql) != "1":
        raise RuntimeError("Rollback changed concurrently")
    state["status"] = "withdrawn-previous-pilot-restored"
    state["withdrawn_at"] = now()
    save(state)
    health()
    print("FLEET_WITHDRAWN; old public release/pilot restored; installed devices not downgraded")


def verify():
    state = load()
    try:
        if latest_id() != state["ota_id"] or current_config() != DISABLED_PILOT:
            raise RuntimeError("Unexpected active release policy")
        verify_image(IMAGE)
        selection_smoke(state)
        download_smoke(state["ota_id"], "public")
        health()
    except Exception:
        rollback()
        raise
    state["status"] = "fleet-active-verified"
    state["verified_at"] = now()
    save(state)
    print("FLEET_VERIFIED", VERSION, "all jiuchuan-s3 MACs; opt-outs unchanged")


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["prepare", "publish", "verify", "rollback"])
    action = parser.parse_args().action
    {"prepare": prepare, "publish": publish, "verify": verify, "rollback": rollback}[action]()
