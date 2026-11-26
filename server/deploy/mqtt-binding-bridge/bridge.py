"""Bridge legacy customer assignments to server-issued, boot-scoped bindings."""
import json
import logging
import os
from pathlib import Path
import queue
import re
import signal
import sqlite3
import ssl
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

LOG = logging.getLogger("mqtt-binding")
FRESH_SECONDS = 180
MAX_PAYLOAD = 32768


class BridgeError(Exception):
    pass


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def parse_payload(payload):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    value = json.loads(payload, object_pairs_hook=unique_object,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError("invalid number")))
    if not isinstance(value, dict):
        raise ValueError("JSON object required")
    return value


def identifier(value, limit=128):
    return isinstance(value, str) and 0 < len(value) <= limit and value == value.strip() and not any(ord(c) < 32 for c in value)


def cab_id(value):
    return isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value) is not None


def business_topic(cab, kind):
    return f"topic/{cab}/{kind}"


BUSINESS_SUBSCRIPTIONS = ((business_topic("+", "online"), 1), (business_topic("+", "cmd"), 1))


def parse_business_topic(topic):
    parts = topic.split("/")
    if len(parts) != 3:
        return None
    namespace, cab, kind = parts
    if namespace != "topic" or cab == "group" or not cab_id(cab) or kind not in ("online", "cmd"):
        return None
    return cab, kind


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise BridgeError("HTTP redirect refused")


def post_json(url, body, token=None, timeout=10):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(url, json.dumps(body, separators=(",", ":")).encode(), headers)
    try:
        with urllib.request.build_opener(NoRedirect()).open(req, timeout=timeout) as response:
            data = response.read(65537)
            if len(data) > 65536:
                raise BridgeError("HTTP response too large")
            return parse_payload(data) if len(data) <= MAX_PAYLOAD else json.loads(data)
    except urllib.error.HTTPError as error:
        raise BridgeError("HTTP status %d" % error.code) from None
    except (urllib.error.URLError, TimeoutError, ValueError, OSError):
        # Exception bodies/URLs may contain credentials; never forward them to logs.
        raise BridgeError("HTTP request failed") from None


class Backend:
    def __init__(self, url, secret):
        self.url = url.rstrip("/")
        self.secret = secret

    def post(self, route, body):
        result = post_json(self.url + "/config/user-binding/" + route, body, self.secret)
        if result.get("code") not in (0, "0"):
            raise BridgeError("backend rejected " + route)
        return result.get("data")


class Store:
    """Only intent and public identity metadata are persisted, never binding tokens."""
    def __init__(self, path):
        if str(path) != ":memory:":
            Path(path).parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.db = sqlite3.connect(path)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        with self.db:
            self.db.execute("BEGIN")
            self.db.execute("""CREATE TABLE IF NOT EXISTS intent (
                cab TEXT PRIMARY KEY, boot TEXT NOT NULL, user TEXT NOT NULL,
                group_id TEXT, request_id TEXT NOT NULL, created REAL NOT NULL,
                next_try REAL NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
                version INTEGER, session_id TEXT,
                group_pending INTEGER NOT NULL DEFAULT 0,
                binding_ready INTEGER NOT NULL DEFAULT 0)""")
            columns = {row[1] for row in self.db.execute("PRAGMA table_info(intent)")}
            if "group_pending" not in columns:
                self.db.execute("ALTER TABLE intent ADD COLUMN group_pending INTEGER NOT NULL DEFAULT 0")
                self.db.execute("UPDATE intent SET group_pending=1 WHERE group_id IS NOT NULL")
            if "binding_ready" not in columns:
                self.db.execute("ALTER TABLE intent ADD COLUMN binding_ready INTEGER NOT NULL DEFAULT 0")
            self.db.execute("""CREATE TABLE IF NOT EXISTS producer_receipt (
                cab TEXT NOT NULL, cmd_id TEXT NOT NULL, boot TEXT NOT NULL,
                user TEXT NOT NULL, group_id TEXT, created REAL NOT NULL,
                PRIMARY KEY(cab,cmd_id))""")
            self.db.execute("""CREATE TABLE IF NOT EXISTS terminal_notification (
                request_id TEXT PRIMARY KEY, cab TEXT NOT NULL,
                state TEXT NOT NULL, message TEXT NOT NULL, created REAL NOT NULL,
                next_try REAL NOT NULL, attempts INTEGER NOT NULL DEFAULT 0)""")
            self.db.execute("CREATE INDEX IF NOT EXISTS terminal_due ON terminal_notification(next_try)")
            if "terminal_state" in columns:
                # Old ready/group-only rows still contain work, not just a notification.
                for row in self.db.execute("SELECT * FROM intent WHERE terminal_state IS NOT NULL").fetchall():
                    if row["terminal_state"] == "ready" and not row["version"] and row["group_id"]:
                        self.db.execute("""UPDATE intent SET binding_ready=1,group_pending=1,
                            terminal_state=NULL,terminal_message=NULL WHERE cab=?""", (row["cab"],))
                    else:
                        self._enqueue_notification(row, row["terminal_state"], row["terminal_message"], row["next_try"])
                        self.db.execute("DELETE FROM intent WHERE cab=?", (row["cab"],))
        if str(path) != ":memory:":
            os.chmod(path, 0o600)

    def get(self, cab):
        return self.db.execute("SELECT * FROM intent WHERE cab=?", (cab,)).fetchone()

    def pending(self):
        return self.db.execute("SELECT * FROM intent ORDER BY created").fetchall()

    def create(self, cab, boot, user, group_id, now, producer_id=None, binding_ready=False):
        with self.db:
            self.db.execute("""INSERT OR REPLACE INTO intent
                (cab,boot,user,group_id,request_id,created,next_try,attempts,group_pending,binding_ready)
                VALUES (?,?,?,?,?,?,?,0,?,?)""",
                (cab, boot, user, group_id, str(uuid.uuid4()), now, now, bool(group_id), binding_ready))
            if producer_id:
                self._remember(cab, producer_id, boot, user, group_id, now)
        return self.get(cab)

    def dispatched(self, cab, version, session):
        self.db.execute("UPDATE intent SET version=?,session_id=? WHERE cab=?", (version, session, cab))
        self.db.commit()

    def update_group(self, cab, group_id, now, producer_id=None):
        with self.db:
            intent = self.get(cab)
            if group_id is not None and group_id != intent["group_id"]:
                self.db.execute("UPDATE intent SET group_id=?,group_pending=1,next_try=? WHERE cab=?",
                                (group_id, now, cab))
            if producer_id:
                self._remember(cab, producer_id, intent["boot"], intent["user"], group_id, now)

    def group_delivered(self, intent):
        with self.db:
            self.db.execute("UPDATE intent SET group_pending=0 WHERE request_id=? AND group_id=?",
                            (intent["request_id"], intent["group_id"]))

    def binding_verified(self, cab):
        with self.db:
            self.db.execute("UPDATE intent SET binding_ready=1 WHERE cab=?", (cab,))

    def receipt(self, cab, cmd_id):
        return self.db.execute("SELECT * FROM producer_receipt WHERE cab=? AND cmd_id=?", (cab, cmd_id)).fetchone()

    def _remember(self, cab, cmd_id, boot, user, group_id, now):
        self.db.execute("INSERT INTO producer_receipt VALUES (?,?,?,?,?,?)", (cab, cmd_id, boot, user, group_id, now))

    def _enqueue_notification(self, intent, state, message, now):
        self.db.execute("""INSERT OR IGNORE INTO terminal_notification
            (request_id,cab,state,message,created,next_try) VALUES (?,?,?,?,?,?)""",
            (intent["request_id"], intent["cab"], state, message, now, now))

    def complete(self, intent, state, message, now):
        with self.db:
            current = self.get(intent["cab"])
            if current and current["request_id"] == intent["request_id"]:
                self._enqueue_notification(intent, state, message, now)
                self.db.execute("DELETE FROM intent WHERE request_id=?", (intent["request_id"],))
        return self.notification(intent["request_id"])

    def notification(self, request_id):
        return self.db.execute("SELECT * FROM terminal_notification WHERE request_id=?", (request_id,)).fetchone()

    def notifications(self, now):
        return self.db.execute("""SELECT * FROM terminal_notification WHERE next_try<=?
            ORDER BY created,request_id LIMIT 64""", (now,)).fetchall()

    def notification_sent(self, request_id):
        with self.db:
            self.db.execute("DELETE FROM terminal_notification WHERE request_id=?", (request_id,))

    def retry_notification(self, request_id, now):
        with self.db:
            self.db.execute("""UPDATE terminal_notification SET next_try=?,attempts=attempts+1
                WHERE request_id=?""", (now + 10, request_id))

    def pending_count(self):
        return self.db.execute("""SELECT (SELECT count(*) FROM intent) +
            (SELECT count(*) FROM terminal_notification)""").fetchone()[0]

    def retry(self, cab, now, delay):
        self.db.execute("UPDATE intent SET attempts=attempts+1,next_try=? WHERE cab=?", (now + delay, cab))
        self.db.commit()

    def delete(self, cab):
        self.db.execute("DELETE FROM intent WHERE cab=?", (cab,))
        self.db.commit()


class Engine:
    def __init__(self, backend, store, publish, clock=time.time):
        self.backend, self.store, self.publish, self.clock = backend, store, publish, clock
        self.online = {}
        self.previous_boots = {}

    def reply(self, cab, state, message, request_id=""):
        self.publish(business_topic(cab, "cmd"), {"bindingAuto": state, "cmdID": request_id, "msg": message})
        LOG.info("binding cab=%s state=%s", cab, state)

    def receive(self, topic, payload, retained=False, received=None):
        if retained:
            return
        now = self.clock()
        received = now if received is None else received
        if now - received > FRESH_SECONDS or received > now + 5:
            return
        route = parse_business_topic(topic)
        if route is None:
            return
        cab, kind = route
        value = parse_payload(payload)
        if kind == "online":
            self._online(cab, value, received)
        else:
            self._assignment(cab, value, now)

    def _online(self, cab, value, received):
        boot = value.get("bootID")
        if value.get("cmd") != "KJ" or value.get("cabGuid") != cab or not identifier(boot):
            return
        previous = self.online.get(cab)
        if boot in self.previous_boots.get(cab, set()):
            return
        # An approved MAC/cabGuid mapping is required at the API; MQTT identity alone is not proof.
        self.backend.post("online", value)
        if previous and previous[0]["bootID"] != boot:
            seen = self.previous_boots.setdefault(cab, set())
            seen.add(previous[0]["bootID"])
            if len(seen) > 64:
                seen.pop()
        self.online[cab] = (value, received)
        intent = self.store.get(cab)
        if not intent:
            return
        if intent["boot"] != boot:
            self.finish(intent, "error", "boot changed; resend the current user's assignment")
        elif (intent["version"] and value.get("bindingVersion") == intent["version"]
              and value.get("usageSessionID") == intent["session_id"]
              and value.get("userGUID") == intent["user"]):
            if value.get("bindingState") == "ready":
                if intent["group_pending"]:
                    self.store.binding_verified(cab)
                else:
                    self.finish(intent, "ready", "device verified current user")
            elif value.get("bindingState") in ("failed", "expired", "error"):
                self.finish(intent, "error", "device rejected user verification")

    def finish(self, intent, state, message):
        notification = self.store.complete(intent, state, message, self.clock())
        if notification:
            self._send_terminal(notification)

    def _send_terminal(self, notification):
        try:
            self.reply(notification["cab"], notification["state"], notification["message"], notification["request_id"])
            self.store.notification_sent(notification["request_id"])
            return True
        except BridgeError:
            self.store.retry_notification(notification["request_id"], self.clock())
            return False

    def _assignment(self, cab, value, now):
        if any(key in value for key in ("bindingAuto", "deviceExecute", "downSDfile")):
            return
        if any(key in value for key in ("cmd", "bootID", "bindingToken", "bindingVersion", "usageSessionID")):
            return
        identities = [value[key] for key in ("userGUID", "userGuid", "user_guid") if key in value]
        if not identities:
            return
        user = identities[0]
        if not identifier(user) or any(other != user for other in identities):
            self.reply(cab, "error", "non-empty, unambiguous userGUID required")
            return
        report = self.online.get(cab)
        if not report or now - report[1] > FRESH_SECONDS:
            self.reply(cab, "error", "fresh mapped online report required; resend after device is online")
            return
        report = report[0]
        group_id = value.get("groupID")
        if group_id is not None and (not isinstance(group_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", group_id)):
            self.reply(cab, "error", "invalid groupID")
            return
        # No-cmdID legacy repeats have no identity that distinguishes replay from new intent.
        producer_id = value.get("cmdID")
        if producer_id is not None:
            if not identifier(producer_id):
                self.reply(cab, "error", "invalid cmdID")
                return
            previous = self.store.receipt(cab, producer_id)
            if previous:
                if (previous["boot"] != report["bootID"] or previous["user"] != user
                    or previous["group_id"] != group_id):
                    self.reply(cab, "error", "cmdID belongs to a different boot or assignment", producer_id)
                return
        intent = self.store.get(cab)
        if intent and intent["boot"] == report["bootID"] and intent["user"] == user:
            self.store.update_group(cab, group_id, now, producer_id)
            return
        if not intent and report.get("userGUID") == user and report.get("bindingState") == "ready":
            self.store.create(cab, report["bootID"], user, group_id, now, producer_id, binding_ready=True)
            self.tick()
            return
        intent = self.store.create(cab, report["bootID"], user, group_id, now, producer_id)
        self.reply(cab, "requested", "waiting for server-issued binding and device verification", intent["request_id"])

    def tick(self):
        for notification in self.store.notifications(self.clock()):
            if not self._send_terminal(notification):
                return
        for intent in self.store.pending():
            now = self.clock()
            cab = intent["cab"]
            if now - intent["created"] > FRESH_SECONDS:
                self.finish(intent, "error", "binding timed out; resend assignment")
                continue
            report = self.online.get(cab)
            if not report or now - report[1] > FRESH_SECONDS or report[0]["bootID"] != intent["boot"]:
                continue
            if intent["next_try"] > now:
                continue
            try:
                if intent["binding_ready"]:
                    current = report[0]
                    if (current.get("userGUID") != intent["user"] or current.get("bindingState") != "ready"
                        or (intent["version"] and (current.get("bindingVersion") != intent["version"]
                            or current.get("usageSessionID") != intent["session_id"]))):
                        self.finish(intent, "error", "binding changed; resend assignment")
                        continue
                    if intent["group_pending"]:
                        self.publish(business_topic(cab, "cmd"), {"groupID": intent["group_id"]})
                    message = "device verified current user" if intent["version"] else "device already reports this user ready"
                    self.finish(intent, "ready", message)
                    continue
                result = self.backend.post("legacy", {"cabGuid": cab, "bootID": intent["boot"],
                    "userGUID": intent["user"], "requestID": intent["request_id"]})
                cmd = result.get("command", {}) if isinstance(result, dict) else {}
                if (cmd.get("cmd") != "bindUser" or cmd.get("bootID") != intent["boot"]
                    or cmd.get("userGUID") != intent["user"]
                    or type(cmd.get("bindingVersion")) is not int or cmd["bindingVersion"] <= 0
                    or not identifier(cmd.get("usageSessionID"))
                    or not identifier(cmd.get("bindingToken")) or len(cmd["bindingToken"]) < 43):
                    raise BridgeError("invalid binding response")
                self.store.dispatched(cab, cmd["bindingVersion"], cmd["usageSessionID"])
                if intent["group_pending"]:
                    # 2.1.7 rejects mixed group/user commands before saving the group.
                    self.publish(business_topic(cab, "cmd"), {"groupID": intent["group_id"]})
                    self.store.group_delivered(intent)
                self.publish(business_topic(cab, "cmd"), cmd)
                if not intent["version"]:
                    self.reply(cab, "dispatched", "command sent; awaiting device verification", intent["request_id"])
                self.store.retry(cab, now, 10)
            except BridgeError:
                self.store.retry(cab, now, min(30, 2 ** min(intent["attempts"] + 1, 5)))
                LOG.warning("binding retry cab=%s attempt=%d", cab, intent["attempts"] + 1)


def credentials(url, account):
    data = post_json(url, {"imei": account})
    if (data.get("success") is not True or data.get("clientId") != account
        or not identifier(data.get("username"), 1024) or not identifier(data.get("password"), 1024)):
        raise BridgeError("invalid service credentials")
    parsed = urllib.parse.urlparse(data.get("wssUrl", ""))
    if parsed.scheme != "wss" or not parsed.hostname or parsed.username or parsed.password:
        raise BridgeError("secure WSS URL required")
    ttl = data.get("expiresIn")
    if type(ttl) is not int or ttl < 120:
        raise BridgeError("invalid credentials expiry")
    return data, parsed, min(ttl - 60, int(ttl * 0.8))


def mqtt_tls_context():
    context = ssl.create_default_context()
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    # This Baidu endpoint negotiates TLS_RSA_WITH_AES_*_GCM; keep CA/hostname checks.
    context.set_ciphers("ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:"
                        "AES256-GCM-SHA384:AES128-GCM-SHA256")
    return context


class Runtime:
    def __init__(self, config, backend, store, clock=time.monotonic):
        self.config = config
        self.clock = clock
        self.events = queue.Queue(maxsize=512)
        self.stopped = threading.Event()
        self.connected = threading.Event()
        self.subscribed = threading.Event()
        self.reconnect_requested = threading.Event()
        self.client = None
        self.engine = Engine(backend, store, self.publish)
        self.last_health = 0
        self.next_connect = 0
        self.reconnect_delay = 2
        self.refresh_at = 0
        self.connect_deadline = 0
        self.subscribed_at = None

    def publish(self, topic, value):
        if not self.connected.is_set() or not self.subscribed.is_set() or self.reconnect_requested.is_set():
            raise BridgeError("MQTT unavailable")
        try:
            info = self.client.publish(topic, json.dumps(value, separators=(",", ":")), qos=1, retain=False)
            info.wait_for_publish(timeout=5)
            if not info.is_published():
                raise BridgeError("MQTT publish timeout")
        except (ValueError, RuntimeError, BridgeError, OSError):
            self.reconnect_requested.set()
            self.disconnect()
            raise BridgeError("MQTT publish failed") from None

    def connect(self):
        import paho.mqtt.client as mqtt
        data, url, refresh = credentials(self.config["credentials_url"], self.config["account"])
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=data["clientId"],
                             transport="websockets", protocol=mqtt.MQTTv311, clean_session=True,
                             reconnect_on_failure=False)
        client.max_queued_messages_set(16)
        client.max_inflight_messages_set(1)
        self.reconnect_requested.clear()
        client.username_pw_set(data["username"], data["password"])
        client.tls_set_context(mqtt_tls_context())
        client.ws_set_options(path=url.path or "/mqtt")

        def on_connect(c, userdata, flags, reason, properties):
            if c is not self.client:
                return
            if reason.is_failure:
                LOG.error("MQTT connection rejected")
                self.reconnect_requested.set()
                return
            self.connected.set()
            rc, _ = c.subscribe(list(BUSINESS_SUBSCRIPTIONS))
            if rc != mqtt.MQTT_ERR_SUCCESS:
                self.reconnect_requested.set()

        def on_subscribe(c, userdata, mid, reasons, properties):
            if c is not self.client:
                return
            if len(reasons) != len(BUSINESS_SUBSCRIPTIONS) or any(reason.is_failure for reason in reasons):
                LOG.error("MQTT business-topic subscription rejected; check account ACL")
                self.subscribed.clear()
                self.reconnect_requested.set()
                return
            self.subscribed_at = self.clock()
            self.subscribed.set()
            LOG.info("MQTT business-topic subscriptions accepted")

        def on_disconnect(c, userdata, flags, reason, properties):
            if c is not self.client:
                return
            self.connected.clear()
            self.subscribed.clear()
            self.reconnect_requested.set()

        def on_message(c, userdata, message):
            if c is not self.client or self.reconnect_requested.is_set() or message.retain or len(message.payload) > MAX_PAYLOAD:
                return
            try:
                self.events.put_nowait((message.topic, message.payload, False, time.time()))
            except queue.Full:
                LOG.error("MQTT receive queue full; message discarded")

        client.on_connect, client.on_subscribe = on_connect, on_subscribe
        client.on_disconnect, client.on_message = on_disconnect, on_message
        self.client = client
        client.connect(url.hostname, url.port or 443, keepalive=30)
        client.loop_start()
        return self.clock() + refresh

    def disconnect(self):
        client, self.client = self.client, None
        self.connected.clear()
        self.subscribed.clear()
        self.subscribed_at = None
        if client:
            try:
                client.disconnect()
            except OSError:
                LOG.warning("MQTT disconnect failed")
            finally:
                client.loop_stop()

    def _schedule_reconnect(self):
        self.disconnect()
        self.reconnect_requested.clear()
        self.next_connect = self.clock() + self.reconnect_delay
        self.reconnect_delay = min(30, self.reconnect_delay * 2)

    def maintain_connection(self):
        now = self.clock()
        if self.reconnect_requested.is_set():
            self._schedule_reconnect()
            return
        if self.client is not None:
            if self.subscribed.is_set():
                # Brief successful connections must not reset a flapping connection's backoff.
                if self.subscribed_at is not None and now - self.subscribed_at >= 30:
                    self.reconnect_delay = 2
            elif now >= self.connect_deadline:
                LOG.warning("MQTT connection/subscription timed out")
                self._schedule_reconnect()
                return
            if now < self.refresh_at:
                return
            self.disconnect()
            self.next_connect = now
        if now < self.next_connect:
            return
        try:
            self.connect_deadline = now + 30
            self.refresh_at = self.connect()
        except (BridgeError, ValueError, OSError):
            LOG.warning("MQTT connection attempt failed")
            self._schedule_reconnect()

    def run(self):
        try:
            while not self.stopped.is_set():
                try:
                    self.maintain_connection()
                    try:
                        event = self.events.get(timeout=0.5)
                        self.engine.receive(*event)
                    except queue.Empty:
                        pass
                    if self.subscribed.is_set() and not self.reconnect_requested.is_set():
                        self.engine.tick()
                    if time.time() - self.last_health >= 10:
                        self.last_health = time.time()
                        health = {"updated": self.last_health, "mqtt": self.connected.is_set(),
                                  "subscribed": self.subscribed.is_set(), "pending": self.engine.store.pending_count()}
                        path = Path(self.config["state_dir"]) / "health.json"
                        temp = path.with_suffix(".tmp")
                        temp.write_text(json.dumps(health), encoding="utf-8")
                        temp.replace(path)
                except (BridgeError, ValueError):
                    LOG.warning("event/request rejected; check mapping, API health and fresh device telemetry")
                except OSError:
                    LOG.error("transport/storage failure")
                    self.stopped.wait(10)
        finally:
            self.disconnect()


def account_lock(state_dir, account):
    import fcntl
    folder = Path(state_dir)
    folder.mkdir(parents=True, exist_ok=True, mode=0o700)
    lock = (folder / (account + ".lock")).open("a")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lock.close()
        raise BridgeError("service account is already in use") from None
    return lock


def main():
    os.umask(0o077)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    config = {"account": os.environ["MQTT_SERVICE_ACCOUNT"],
              "credentials_url": os.environ["MQTT_CREDENTIALS_URL"],
              "state_dir": os.getenv("STATE_DIR", "/state")}
    # Current customer endpoint is HTTP; opt in explicitly, never silently downgrade HTTPS.
    if urllib.parse.urlparse(config["credentials_url"]).scheme != "https" and os.getenv("ALLOW_HTTP_CREDENTIALS") != "1":
        raise BridgeError("HTTP credentials endpoint requires explicit configuration")
    secret = Path(os.environ["API_SECRET_FILE"]).read_text(encoding="utf-8").strip()
    if not secret:
        raise BridgeError("missing backend secret")
    account_guard = account_lock(config["state_dir"], config["account"])
    runtime = Runtime(config, Backend(os.environ["API_BASE_URL"], secret),
                      Store(str(Path(config["state_dir"]) / "intent.sqlite3")))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: runtime.stopped.set())
    runtime.run()


if __name__ == "__main__":
    main()
