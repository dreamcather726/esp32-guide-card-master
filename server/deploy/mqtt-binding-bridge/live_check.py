"""Explicit one-device acceptance test. Stop daemon first; shares its account lock."""
import argparse
import json
import logging
import os
from pathlib import Path
import queue
import time

from bridge import Backend, BridgeError, Runtime, Store, account_lock, business_topic, parse_payload


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cab-guid", required=True)
    parser.add_argument("--user-guid", required=True)
    parser.add_argument("--group-id", required=True)
    args = parser.parse_args()
    os.umask(0o077)
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    config = {"account": os.environ["MQTT_SERVICE_ACCOUNT"], "credentials_url": os.environ["MQTT_CREDENTIALS_URL"],
              "state_dir": os.environ["STATE_DIR"]}
    guard = account_lock(config["state_dir"], config["account"])
    store = Store(str(Path(config["state_dir"]) / "intent.sqlite3"))
    backend = Backend(os.environ["API_BASE_URL"], Path(os.environ["API_SECRET_FILE"]).read_text().strip())
    runtime = Runtime(config, backend, store)
    sent = False
    verified = False
    verified_identity = None
    try:
        runtime.connect()
        deadline = time.monotonic() + 160
        while time.monotonic() < deadline:
            try:
                event = runtime.events.get(timeout=0.5)
                runtime.engine.receive(*event)
                if event[0] == business_topic(args.cab_guid, "online"):
                    data = parse_payload(event[1])
                    if data.get("cabGuid") == args.cab_guid:
                        print("DEVICE", {k: data.get(k) for k in ("cabGuid", "bootID", "groupID", "userGUID", "bindingState", "bindingVersion")}, flush=True)
                        if (sent and data.get("userGUID") == args.user_guid and data.get("groupID") == args.group_id
                            and data.get("bindingState") == "ready"):
                            identity = (data.get("bindingVersion"), data.get("usageSessionID"))
                            if verified_identity is None:
                                verified_identity = identity
                                print("PHYSICAL_DEVICE_READY", flush=True)
                                runtime.publish(business_topic(args.cab_guid, "cmd"), {"groupID": args.group_id, "userGUID": args.user_guid})
                                print("DUPLICATE_ORIGINAL_COMMAND_PUBLISHED", flush=True)
                            else:
                                assert identity == verified_identity, "Duplicate assignment rotated binding"
                                verified = True
                                print("DUPLICATE_IDEMPOTENCY_OK", flush=True)
                                break
            except queue.Empty:
                pass
            if not sent and args.cab_guid in runtime.engine.online and runtime.subscribed.is_set():
                runtime.publish(business_topic(args.cab_guid, "cmd"), {"groupID": args.group_id, "userGUID": args.user_guid})
                sent = True
                print("ORIGINAL_TWO_FIELD_COMMAND_PUBLISHED", flush=True)
            if runtime.subscribed.is_set():
                runtime.engine.tick()
            if runtime.reconnect_requested.is_set():
                raise BridgeError("Connection lost during acceptance check")
        if not verified:
            raise BridgeError("Device ready was not observed during this check")
    finally:
        runtime.disconnect()
        store.db.close()


if __name__ == "__main__":
    main()
