"""Owner-scoped repair of the exact hardware mapping supplied by the customer."""
import argparse
import datetime
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, "/opt/xiaozhi-server/verify-20261007-user-switch")
from smoke_api import api, database, sql

MAC = "7c:4f:ad:24:80:e4"
CAB = "864479086851160s3"
OWNER = 2063542642570055681
STATE = Path(__file__).parent / "mapping-repair-state.json"
ROUTE = "/device/" + MAC + "/mqtt-binding"


def now():
    return datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat()


def owner_token():
    identity = database("SELECT id,user_id FROM ai_device WHERE mac_address=" + sql(MAC))
    if identity != MAC + "\t" + str(OWNER):
        raise RuntimeError("Device identity/owner changed; stop without changing mapping")
    tokens = database("SELECT token FROM sys_user_token WHERE user_id=" + str(OWNER) +
                      " AND expire_date>NOW() ORDER BY expire_date DESC LIMIT 1").splitlines()
    if len(tokens) != 1 or not tokens[0]:
        raise RuntimeError("An active owner login is required")
    return tokens[0]


def snapshot(view):
    report = view.get("report") or {}
    return {"macAddress": view.get("macAddress"), "cabGuid": view.get("cabGuid"),
            "fresh": view.get("fresh"), "receivedAt": view.get("receivedAt"),
            "report": {key: report.get(key) for key in (
                "macAddress", "cabGuid", "bootID", "userGUID", "groupID", "bindingState",
                "bindingVersion", "usageSessionID", "fwVersion")}}


def apply():
    token = owner_token()
    before = api("GET", ROUTE, token)
    if before.get("cabGuid") not in (None, "", CAB):
        raise RuntimeError("Device has a different mapping; do not overwrite it")
    occupied = database("SELECT mac_address FROM ai_mqtt_binding_mapping WHERE cab_guid=" + sql(CAB))
    if occupied and occupied != MAC:
        raise RuntimeError("Cabinet already belongs to another device")
    if STATE.exists():
        raise RuntimeError("Repair record exists; use observe instead of repeating apply")
    state = {"startedAt": now(), "requestedPair": {"macAddress": MAC, "cabGuid": CAB},
             "before": snapshot(before), "status": "prepared"}
    STATE.write_text(json.dumps(state, indent=2) + "\n")
    api("PUT", ROUTE, token, {"cabGuid": CAB})
    after = api("GET", ROUTE, token)
    if after.get("cabGuid") != CAB or after.get("macAddress") != MAC:
        raise RuntimeError("Mapping readback mismatch")
    state.update(status="mapping-saved", savedAt=now(), after=snapshot(after))
    STATE.write_text(json.dumps(state, indent=2) + "\n")
    print("MAPPING_SAVED", MAC, CAB, flush=True)
    print("No user assignment, profile modification, MQTT publication or service restart", flush=True)


def observe():
    token = owner_token()
    for attempt in range(10):
        view = api("GET", ROUTE, token)
        if view.get("cabGuid") != CAB:
            raise RuntimeError("Mapping changed; stop observation")
        if view.get("fresh") and (view.get("report") or {}).get("bootID"):
            observed = snapshot(view)
            state = json.loads(STATE.read_text())
            state.update(status="fresh-live-report-received", observedAt=now(), observed=observed)
            STATE.write_text(json.dumps(state, indent=2) + "\n")
            print("LIVE_REPORT", json.dumps(observed), flush=True)
            return
        if attempt < 9:
            time.sleep(10)
    print("MAPPING_SAVED_WAITING_FOR_DEVICE_TELEMETRY", flush=True)


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("apply", "observe"))
    action = parser.parse_args().action
    {"apply": apply, "observe": observe}[action]()
