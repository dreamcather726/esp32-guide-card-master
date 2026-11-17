"""Run as the deployment owner; never publish a pilot image as a fleet release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.request
import uuid
import zipfile

ROOT = Path("/opt/xiaozhi-server")
DEPLOY = ROOT / "deploy-20261006-ota-pilot"
DB = "xiaozhi_esp32_server"
MAC = "7c:4f:ad:24:81:fc"
CAB = "864479086586824s3"
VERSION = "2.1.9"
IMAGE_NAME = "vanex-s3-2.1.9-202227aedb75.bin"
IMAGE_SHA = "202227aedb752c2a99a43e4a28426f54d7d2e2e148968f2a9e0ff9d2516edf2b"
IMAGE_SIZE = 3227232
FIXTURES = ("02:fa:06:21:90:01", "02:fa:06:21:90:02")
NO_UPDATE_URL = "http://xiaozhi.server.com:8002/xiaozhi/otaMag/download/NOT_ACTIVATED_FIRMWARE_THIS_IS_A_INVALID_URL"


def command(args, data=None):
    result = subprocess.run(args, input=data, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError("Deployment command failed; output omitted")
    return result.stdout.strip()


def database(sql):
    return command(["docker", "exec", "-i", "xiaozhi-esp32-server-db", "sh", "-lc",
                    'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -uroot -N -B ' + DB], sql)


def literal(value):
    return "CONVERT(0x" + str(value).encode().hex() + " USING utf8mb4)"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def state():
    return json.loads((DEPLOY / "state.json").read_text())


def save_state(value):
    (DEPLOY / "state.json").write_text(json.dumps(value, indent=2) + "\n")


def redis(*args):
    container = "xiaozhi-esp32-server-redis"
    config = json.loads(command(["docker", "inspect", container]))[0]["Config"]
    cmd = config.get("Cmd") or []
    password = cmd[cmd.index("--requirepass") + 1] if "--requirepass" in cmd else ""
    script = 'read -r REDISCLI_AUTH; export REDISCLI_AUTH; exec redis-cli --raw "$@"'
    result = command(["docker", "exec", "-i", container, "sh", "-c", script, "sh", *args], password + "\n")
    if result.startswith(("ERR", "NOAUTH", "WRONGPASS")):
        raise RuntimeError("Redis operation failed")
    return result


def pilot_config(enabled, targets):
    current = state()
    value = json.dumps({"enabled": enabled, "boardType": "jiuchuan-s3", "otaId": current["ota_id"],
                        "macAddresses": targets}, separators=(",", ":"))
    database("UPDATE sys_params SET param_value=" + literal(value) + ",update_date=NOW() "
             "WHERE param_code='server.ota_pilot'")
    if database("SELECT param_value FROM sys_params WHERE param_code='server.ota_pilot'") != value:
        raise RuntimeError("Pilot configuration update was not applied")


def stage():
    if (DEPLOY / "state.json").exists():
        raise RuntimeError("Staging already exists; inspect before retrying")
    uploaded = DEPLOY / "xiaozhi-2.1.9.bin"
    image = uploaded.read_bytes()
    if len(image) != IMAGE_SIZE or digest(uploaded) != IMAGE_SHA or image[0] != 0xe9 or image[48:80].split(b"\0")[0] != b"2.1.9":
        raise RuntimeError("Application image identity mismatch")
    if int.from_bytes(image[12:14], "little") != 9 or len(image) > 0x3f0000:
        raise RuntimeError("Chip or OTA partition size mismatch")
    mapping = database("SELECT mac_address FROM ai_mqtt_binding_mapping WHERE cab_guid=" + literal(CAB))
    if mapping != MAC:
        raise RuntimeError("Confirmed customer device mapping changed")
    if database("SELECT COUNT(*) FROM sys_params WHERE param_code='server.ota_pilot'") != "0":
        raise RuntimeError("Existing rollout config must not be overwritten")
    owner = database("SELECT user_id FROM ai_device WHERE mac_address=" + literal(MAC))
    if not owner.isdigit():
        raise RuntimeError("Target device ownership is ambiguous")
    if database("SELECT COUNT(*) FROM ai_ota WHERE type='pilot-jiuchuan-s3' AND version='2.1.9'") != "0":
        raise RuntimeError("Existing pilot image must not be overwritten")
    backup = DEPLOY / "backup"
    backup.mkdir(mode=0o700, exist_ok=False)
    shutil.copy2(ROOT / "manager-api/xiaozhi-esp32-api.jar", backup / "api.jar")
    (backup / "ota-before.tsv").write_text(database("SELECT id,type,version,size,firmware_path,update_date FROM ai_ota ORDER BY id") + "\n")
    before = database("SELECT id,mac_address,app_version,auto_update,last_connected_at FROM ai_device WHERE mac_address=" + literal(MAC))
    (backup / "target-before.tsv").write_text(before + "\n")
    target_file = ROOT / "uploadfile" / IMAGE_NAME
    if target_file.exists() and digest(target_file) != IMAGE_SHA:
        raise RuntimeError("Immutable firmware path already contains different bytes")
    if not target_file.exists():
        shutil.copy2(uploaded, target_file)
        os.chmod(target_file, 0o644)
    rollout = {"ota_id": uuid.uuid4().hex, "version": VERSION, "target_mac": MAC, "target_cab": CAB,
               "sha256": IMAGE_SHA, "image_size": IMAGE_SIZE, "public_release_unchanged": True,
               "status": "staging", "api_before_sha256": digest(backup / "api.jar")}
    save_state(rollout)
    config = json.dumps({"enabled": False, "boardType": "jiuchuan-s3", "otaId": rollout["ota_id"], "macAddresses": [MAC]}, separators=(",", ":"))
    param_id = int.from_bytes(os.urandom(7), "big")
    sql = "START TRANSACTION;\n"
    sql += "INSERT INTO ai_ota(id,firmware_name,type,version,size,remark,firmware_path,sort,creator,create_date,updater,update_date) VALUES (" + ",".join([
        literal(rollout["ota_id"]), literal("Vanex S3 2.1.9 - single device pilot"), literal("pilot-jiuchuan-s3"),
        literal(VERSION), str(IMAGE_SIZE), literal("Target cabGuid " + CAB + "; immutable SHA256 " + IMAGE_SHA),
        literal("/uploadfile/" + IMAGE_NAME), "0", owner, "NOW()", owner, "NOW()"
    ]) + ");\n"
    sql += "INSERT INTO sys_params(id,param_code,param_value,value_type,param_type,remark,creator,create_date,updater,update_date) VALUES (" + ",".join([
        str(param_id), literal("server.ota_pilot"), literal(config), literal("string"), "1",
        literal("Single-device OTA; keep disabled until download/selection smoke passes"), owner, "NOW()", owner, "NOW()"
    ]) + ");\nCOMMIT;"
    database(sql)
    rollout["status"] = "staged-disabled"
    save_state(rollout)
    print("STAGED_DISABLED", rollout["ota_id"], VERSION, IMAGE_SIZE, IMAGE_SHA)
    print("TARGET_BEFORE", before)


def health():
    for _ in range(90):
        try:
            with urllib.request.urlopen("http://127.0.0.1:8004/xiaozhi/ota/", timeout=3) as response:
                if response.status == 200 and "\u8fd0\u884c\u6b63\u5e38" in response.read().decode("utf-8"):
                    return
        except Exception:
            time.sleep(2)
    raise RuntimeError("Management API did not recover")


def install():
    rollout = state()
    new = DEPLOY / "xiaozhi-esp32-api.jar"
    expected = (DEPLOY / "api.sha256").read_text().strip().lower()
    if digest(new) != expected:
        raise RuntimeError("API JAR upload digest mismatch")
    with zipfile.ZipFile(new) as jar:
        if "BOOT-INF/classes/xiaozhi/modules/device/service/PilotOtaPolicy.class" not in jar.namelist():
            raise RuntimeError("Pilot implementation absent from API JAR")
    pilot_config(False, [MAC])
    target = ROOT / "manager-api/xiaozhi-esp32-api.jar"
    command(["docker", "stop", "xiaozhi-esp32-server-web"])
    try:
        shutil.copy2(new, target)
        command(["docker", "start", "xiaozhi-esp32-server-web"])
        health()
    except Exception:
        command(["docker", "stop", "xiaozhi-esp32-server-web"])
        shutil.copy2(DEPLOY / "backup/api.jar", target)
        command(["docker", "start", "xiaozhi-esp32-server-web"])
        health()
        raise
    rollout["api_sha256"] = expected
    rollout["status"] = "api-installed-pilot-disabled"
    save_state(rollout)
    print("API_HEALTH_OK; pilot remains disabled; voice container unchanged")


def post_check(mac, version, base="https://robot.suji.work", board="jiuchuan-s3"):
    payload = json.dumps({"application": {"version": version}, "board": {"type": board}}).encode()
    request = urllib.request.Request(base + "/xiaozhi/ota/", data=payload, headers={
        "Content-Type": "application/json", "Device-Id": mac, "Client-Id": "vanex-ota-pilot-smoke"})
    with urllib.request.urlopen(request, timeout=45) as response:
        return json.load(response)


def verify_selection():
    rollout = state()
    ids = [uuid.uuid4().hex for _ in FIXTURES]
    tickets = []
    fixtures_created = False
    try:
        for mac in FIXTURES:
            if database("SELECT COUNT(*) FROM ai_device WHERE mac_address=" + literal(mac)) != "0":
                raise RuntimeError("Fixture MAC collision; no customer row may be reused")
        sql = "START TRANSACTION;\n"
        for item, mac in zip(ids, FIXTURES):
            sql += "INSERT INTO ai_device(id,mac_address,board,app_version,auto_update,alias) VALUES (" + ",".join([
                literal(item), literal(mac), literal("jiuchuan-s3"), literal("2.1.0"), "1", literal("temporary OTA pilot smoke")
            ]) + ");\n"
        database(sql + "COMMIT;")
        fixtures_created = True
        pilot_config(True, [FIXTURES[0]])
        for base in ("https://robot.suji.work", "http://47.94.91.32:8002"):
            old = post_check(FIXTURES[0], "2.1.0", base)["firmware"]
            assert old["version"] == VERSION and old["url"].startswith("https://robot.suji.work/xiaozhi/otaMag/download/")
            tickets.append(old["url"].rsplit("/", 1)[-1])
            normal = post_check(FIXTURES[1], "2.1.0", base)["firmware"]
            assert normal["version"] == "2.1.3" and normal["url"].startswith("https://robot.suji.work/xiaozhi/otaMag/download/")
            tickets.append(normal["url"].rsplit("/", 1)[-1])
            for version in ("2.1.9", "2.1.10"):
                current = post_check(FIXTURES[0], version, base)["firmware"]
                assert current.get("url") == NO_UPDATE_URL
            assert "firmware" not in post_check(FIXTURES[1], "2.1.0", base, "pilot-jiuchuan-s3")
            database("UPDATE ai_device SET auto_update=0 WHERE id=" + literal(ids[0]))
            assert "firmware" not in post_check(FIXTURES[0], "2.1.0", base)
            database("UPDATE ai_device SET auto_update=1 WHERE id=" + literal(ids[0]))
            print("LIVE_SELECTION_OK", base, "pilot=2.1.9; other=2.1.3; no downgrade; opt-out respected")
        rollout["selection_verified"] = True
        save_state(rollout)
    finally:
        pilot_config(False, [MAC])
        if fixtures_created:
            time.sleep(1)
            for item, mac in zip(ids, FIXTURES):
                database("DELETE FROM ai_device WHERE id=" + literal(item) + " AND mac_address=" + literal(mac))
        for ticket in tickets:
            redis("DEL", "ota:id:" + ticket, "ota:download:count:" + ticket)
        print("Selection fixtures/tickets removed; real device state not simulated")


def activate():
    rollout = state()
    if not rollout.get("selection_verified") or not (DEPLOY / "download-verified.json").is_file():
        raise RuntimeError("Live selection/download verification is required")
    verified = json.loads((DEPLOY / "download-verified.json").read_text())
    if (verified.get("sha256") != IMAGE_SHA or verified.get("ota_id") != rollout["ota_id"] or
            verified.get("api_sha256") != digest(ROOT / "manager-api/xiaozhi-esp32-api.jar") or
            verified.get("endpoints") != ["https://robot.suji.work", "http://47.94.91.32:8002"]):
        raise RuntimeError("Download verification does not match current deployment")
    if database("SELECT auto_update FROM ai_device WHERE mac_address=" + literal(MAC)) != "1":
        raise RuntimeError("Target automatic updates are disabled; do not override owner setting")
    if digest(ROOT / "uploadfile" / IMAGE_NAME) != IMAGE_SHA:
        raise RuntimeError("Published file changed")
    pilot_config(True, [MAC])
    rollout["status"] = "active-for-one-device"
    rollout["activated_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    save_state(rollout)
    print("PILOT_ACTIVE", CAB, MAC, VERSION)
    print("No forced restart or fleet-wide push sent. Wait for real device version check.")


def disable():
    pilot_config(False, [MAC])
    rollout = state()
    rollout["status"] = "disabled"
    save_state(rollout)
    print("PILOT_DISABLED; normal release unchanged")


def verify_download():
    rollout = state()
    image = ROOT / "uploadfile" / IMAGE_NAME
    if digest(image) != IMAGE_SHA:
        raise RuntimeError("Staged firmware changed")
    endpoints = ["https://robot.suji.work", "http://47.94.91.32:8002"]
    for base in endpoints:
        env = dict(os.environ, OTA_SMOKE_BASE=base, OTA_SMOKE_IMAGE=str(image), OTA_SMOKE_ID=rollout["ota_id"])
        result = subprocess.run([sys.executable, str(DEPLOY / "smoke_download.py")], env=env,
                                capture_output=True, text=True, timeout=600)
        (DEPLOY / ("download-https.log" if base.startswith("https:") else "download-legacy.log")).write_text(
            result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError("Download smoke failed for " + base + "; inspect protected deployment log")
        print("DOWNLOAD_VERIFIED", base, result.stdout.strip())
    verified = {"sha256": IMAGE_SHA, "ota_id": rollout["ota_id"], "endpoints": endpoints,
                "api_sha256": digest(ROOT / "manager-api/xiaozhi-esp32-api.jar"),
                "verified_at": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
    (DEPLOY / "download-verified.json").write_text(json.dumps(verified, indent=2) + "\n")


def rollback():
    rollout = state()
    target = ROOT / "manager-api/xiaozhi-esp32-api.jar"
    backup = DEPLOY / "backup/api.jar"
    if digest(backup) != rollout["api_before_sha256"]:
        raise RuntimeError("Backup digest mismatch")
    if digest(target) not in (rollout["api_before_sha256"], rollout.get("api_sha256")):
        raise RuntimeError("API changed since deployment; do not overwrite another release")
    disable()
    command(["docker", "stop", "xiaozhi-esp32-server-web"])
    shutil.copy2(backup, target)
    command(["docker", "start", "xiaozhi-esp32-server-web"])
    health()
    rollout["status"] = "api-rolled-back-pilot-disabled"
    save_state(rollout)
    print("API_ROLLBACK_HEALTH_OK; firmware bytes retained for audit; public release unchanged")


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["stage", "install", "verify-selection", "verify-download", "activate", "disable", "rollback"])
    args = parser.parse_args()
    actions = {"stage": stage, "install": install, "verify-selection": verify_selection,
               "verify-download": verify_download, "activate": activate, "disable": disable, "rollback": rollback}
    actions[args.action]()
