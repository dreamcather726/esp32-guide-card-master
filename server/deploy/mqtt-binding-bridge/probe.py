"""Read-only broker probe. No device commands, secrets or user payloads printed."""
import os
import ssl
import threading
import time
import paho.mqtt.client as mqtt
from bridge import BUSINESS_SUBSCRIPTIONS, account_lock, credentials, mqtt_tls_context, parse_business_topic, parse_payload

account_guard = account_lock(os.environ["STATE_DIR"], os.environ["MQTT_SERVICE_ACCOUNT"])
data, url, refresh = credentials(os.environ["MQTT_CREDENTIALS_URL"], os.environ["MQTT_SERVICE_ACCOUNT"])
connected = threading.Event()
subscribed = threading.Event()
counts = {"online": 0, "commands": 0}
devices = {}
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=data["clientId"], transport="websockets")
client.username_pw_set(data["username"], data["password"])
client.tls_set_context(mqtt_tls_context())
client.ws_set_options(path=url.path or "/mqtt")


def on_connect(c, userdata, flags, reason, properties):
    print("MQTT_CONNECT", reason, flush=True)
    if not reason.is_failure:
        connected.set()
        c.subscribe(list(BUSINESS_SUBSCRIPTIONS))


def on_subscribe(c, userdata, mid, reasons, properties):
    print("BUSINESS_SUBACK", [str(reason) for reason in reasons], flush=True)
    if len(reasons) == len(BUSINESS_SUBSCRIPTIONS) and not any(r.is_failure for r in reasons):
        subscribed.set()


def on_message(c, userdata, message):
    if message.retain:
        return
    route = parse_business_topic(message.topic)
    if route is None:
        return
    cab, kind = route
    if kind == "online":
        counts["online"] += 1
        try:
            value = parse_payload(message.payload)
            if value.get("cabGuid") == cab:
                devices[cab] = {key: value.get(key) for key in ("cabGuid", "macAddress", "bootID", "fwVersion", "bindingState")}
        except ValueError:
            pass
    else:
        counts["commands"] += 1


client.on_connect, client.on_subscribe, client.on_message = on_connect, on_subscribe, on_message
client.connect(url.hostname, url.port or 443, 30)
client.loop_start()
try:
    assert connected.wait(15), "MQTT connect failed"
    assert subscribed.wait(15), "Business topic ACL rejected"
    time.sleep(int(os.getenv("PROBE_SECONDS", "40")))
    print("COUNTS", counts)
    print("DEVICES", list(devices.values()))
finally:
    client.disconnect()
    client.loop_stop()
