"""Run explicitly on the deployment host, never prints credentials."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

ROOT = Path("/opt/xiaozhi-server")
DEPLOY = ROOT / "deploy-20261004-mqtt-binding"
DB = "xiaozhi_esp32_server"


def database(statement):
    result = subprocess.run(["docker", "exec", "-i", "xiaozhi-esp32-server-db", "sh", "-lc",
        'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -uroot -N -B ' + DB],
        input=statement, text=True, capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError("Database operation failed (credentials redacted)")
    return result.stdout.strip()


def secret():
    return subprocess.check_output(["docker", "exec", "xiaozhi-esp32-server", "python", "-c",
        "import yaml; print(yaml.safe_load(open('data/.config.yaml'))['manager-api']['secret'])"], text=True).strip()


def inventory():
    print(database("SELECT mac_address,app_version,alias,last_connected_at FROM ai_device "
                   "WHERE mac_address='7c:4f:ad:24:81:fc'"))
    print("DEVICE_COUNT", database("SELECT COUNT(*) FROM ai_device"))
    print("CURRENT_SESSIONS", database("SELECT COUNT(*) FROM ai_usage_session WHERE revoked_at IS NULL"))


def backup():
    dest = DEPLOY / "backup"
    dest.mkdir(parents=True, exist_ok=True, mode=0o700)
    jar = dest / "xiaozhi-esp32-api.jar"
    if not jar.exists():
        shutil.copy2(ROOT / "manager-api/xiaozhi-esp32-api.jar", jar)
    web = dest / "manager-web.tar.gz"
    if not web.exists():
        with tarfile.open(web, "w:gz") as archive:
            archive.add(ROOT / "manager-web", arcname="manager-web")
    dump = dest / "database.sql"
    if not dump.exists():
        with dump.open("wb") as output:
            result = subprocess.run(["docker", "exec", "xiaozhi-esp32-server-db", "sh", "-lc",
                'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysqldump -uroot --single-transaction --no-tablespaces ' + DB],
                stdout=output, stderr=subprocess.PIPE)
            if result.returncode:
                dump.unlink()
                raise RuntimeError("Database backup failed")
    print("BACKUP_READY", dest)


def configure():
    folder = DEPLOY / "bridge"
    folder.mkdir(parents=True, exist_ok=True, mode=0o700)
    (folder / "api-secret").write_text(secret(), encoding="utf-8")
    os.chmod(folder / "api-secret", 0o600)
    (folder / "bridge.env").write_text("\n".join((
        "MQTT_SERVICE_ACCOUNT=AB12cd34",
        "MQTT_CREDENTIALS_URL=http://mqttak.suji.work:8443/api/credentials",
        "ALLOW_HTTP_CREDENTIALS=1", "API_BASE_URL=http://127.0.0.1:8004/xiaozhi",
        "API_SECRET_FILE=/run/secrets/api-secret", "STATE_DIR=/state", "")), encoding="utf-8")
    os.chmod(folder / "bridge.env", 0o600)
    (folder / "state").mkdir(mode=0o700, exist_ok=True)
    print("BRIDGE_CONFIG_READY (secrets not printed)")


def register_customer_device():
    # This one pairing is evidenced by the customer's screenshot + IMEI log,
    # and the existing registered device alias MEI 86824. Never auto-pair other MACs.
    mac, cab = "7c:4f:ad:24:81:fc", "864479086586824s3"
    rows = database("SELECT id,user_id,alias FROM ai_device WHERE mac_address='" + mac + "'").splitlines()
    if len(rows) != 1 or rows[0].split("\t")[2] != "MEI 86824":
        raise RuntimeError("Customer device verification changed; inspect before registering")
    existing = database("SELECT cab_guid FROM ai_mqtt_binding_mapping WHERE mac_address='" + mac + "'")
    if existing and existing != cab:
        raise RuntimeError("Existing mapping differs; refusing overwrite")
    if not existing:
        database("INSERT INTO ai_mqtt_binding_mapping(mac_address,cab_guid,mapping_id,device_id,owner_id) "
                 "SELECT mac_address,'" + cab + "',UUID(),id,user_id FROM ai_device WHERE mac_address='" + mac + "'")
    print("REGISTERED_PAIR", mac, cab)


def deploy_api():
    backup()
    new = DEPLOY / "xiaozhi-esp32-api.jar"
    if new.stat().st_size < 1000000:
        raise RuntimeError("Invalid JAR")
    target = ROOT / "manager-api/xiaozhi-esp32-api.jar"
    subprocess.run(["docker", "stop", "xiaozhi-esp32-server-web"], check=True)
    shutil.copy2(new, target)
    subprocess.run(["docker", "start", "xiaozhi-esp32-server-web"], check=True)
    for _ in range(60):
        try:
            with urllib.request.urlopen("http://127.0.0.1:8004/xiaozhi/health/hello", timeout=3) as response:
                if response.status == 200:
                    print("API_HEALTH_OK")
                    return
        except Exception:
            time.sleep(2)
    raise RuntimeError("API health failed; restore backup JAR and restart web container")


def deploy_web():
    backup()
    staging = DEPLOY / "web-dist"
    if not (staging / "index.html").is_file():
        raise RuntimeError("Missing staged index.html")
    # Keep old hashed assets for already-open clients, replace index last.
    for entry in staging.iterdir():
        if entry.name == "index.html":
            continue
        if entry.is_dir():
            shutil.copytree(entry, ROOT / "manager-web" / entry.name, dirs_exist_ok=True)
        else:
            shutil.copy2(entry, ROOT / "manager-web" / entry.name)
    shutil.copy2(staging / "index.html", ROOT / "manager-web/index.html")
    print("WEB_DEPLOYED")


if __name__ == "__main__":
    os.umask(0o077)
    actions = {"inventory": inventory, "backup": backup, "configure": configure,
               "deploy-api": deploy_api, "deploy-web": deploy_web, "register-device": register_customer_device}
    actions[sys.argv[1]]()
