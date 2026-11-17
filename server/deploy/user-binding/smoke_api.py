"""Run on the deployment host. Creates and always removes dedicated test fixtures."""
import json
import os
import secrets
import subprocess
import time
import urllib.error
import urllib.request
import uuid
from concurrent.futures import ThreadPoolExecutor

BASE = os.environ.get("BINDING_API_BASE", "http://127.0.0.1:8004/xiaozhi")
DB = "xiaozhi_esp32_server"


def sql(value):
    if value is None:
        return "NULL"
    if type(value) is int:
        return str(value)
    return "CONVERT(0x" + str(value).encode().hex() + " USING utf8mb4)"


def database(statement):
    result = subprocess.run(["docker", "exec", "-i", "xiaozhi-esp32-server-db", "sh", "-lc",
        'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -uroot -N -B ' + DB],
        input=statement, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError("Fixture database operation failed (no credentials logged)")
    return result.stdout.strip()


def clone(table, old_id, new_id, overrides):
    columns = database("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=" +
        sql(DB) + " AND TABLE_NAME=" + sql(table) + " ORDER BY ORDINAL_POSITION").splitlines()
    overrides = dict(overrides, id=new_id)
    names = ",".join("`" + name + "`" for name in columns)
    values = ",".join(sql(overrides[name]) if name in overrides else "`" + name + "`" for name in columns)
    database(f"INSERT INTO `{table}` ({names}) SELECT {values} FROM `{table}` WHERE id={sql(old_id)}")


def api(method, path, token, body=None, ok=True):
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(BASE + path, data=data, method=method,
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            result = json.load(response)
    except urllib.error.HTTPError as exc:
        if not ok:
            return None
        raise RuntimeError(f"HTTP smoke failed: {method} {path} status={exc.code}") from None
    if ok:
        if result.get("code") != 0:
            raise RuntimeError(f"API smoke failed: {method} {path} code={result.get('code')} msg={result.get('msg')}")
        return result.get("data")
    assert result.get("code") != 0, "Expected rejection"
    return None


def websocket_smoke(mac, binding):
    program = r'''
import asyncio, json, sys, time, uuid, yaml, websockets
from core.auth import AuthManager
data=json.load(sys.stdin)
config=yaml.safe_load(open('data/.config.yaml'))
secret=config.get('server',{}).get('auth_key') or config['manager-api']['secret']
client=str(uuid.uuid4())
headers={'Device-Id':data['mac'],'Client-Id':client,'User-Binding':'1',
    'Authorization':'Bearer '+AuthManager(secret).generate_token(client,data['mac'])}
hello=dict(data['binding']['command'],type='hello',transport='websocket',version=1,
    features={'user_binding':True},audio_params={'format':'opus','sample_rate':16000,'channels':1,'frame_duration':60})
async def run():
    async with websockets.connect('ws://127.0.0.1:8000/xiaozhi/v1/',additional_headers=headers) as ws:
        await ws.send(json.dumps(hello))
        started=time.monotonic()
        ready=False
        for _ in range(20):
            message=json.loads(await asyncio.wait_for(ws.recv(),30))
            if message.get('type')=='user_context':
                assert message.get('state')=='ready', 'context rejected: '+str(message.get('code'))
                assert message['usageSessionID']==data['binding']['usageSessionID']
                ready=True
                break
        assert ready
        print('WEBSOCKET_READY seconds=%.2f' % (time.monotonic()-started))
        await ws.send(json.dumps({'type':'listen','state':'detect','text':'This is a deployment test. Please reply briefly: Test successful.'}))
        frames=0
        saw_stt=False
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            response=await asyncio.wait_for(ws.recv(),45)
            if isinstance(response,bytes):
                frames+=1
                continue
            message=json.loads(response)
            saw_stt |= message.get('type')=='stt'
            if message.get('type')=='tts' and message.get('state')=='stop':
                break
        assert saw_stt, 'recognized text not acknowledged'
        assert frames>0, 'no generated audio received'
        print('WEBSOCKET_TEXT_AND_AUDIO_OK frames=%d' % frames)
asyncio.run(run())
'''
    result = subprocess.run(["docker", "exec", "-i", "xiaozhi-esp32-server", "python", "-c", program],
        input=json.dumps({"mac": mac, "binding": binding}), text=True, capture_output=True, timeout=100)
    if result.returncode:
        raise RuntimeError("WebSocket smoke failed: " + result.stderr[-1800:])
    print(result.stdout.strip())


def main():
    marker = uuid.uuid4().hex
    owner = int(time.time() * 1000000)
    other = owner + 1
    tokens = [secrets.token_hex(32), secrets.token_hex(32)]
    agent_id, device_id = uuid.uuid4().hex, uuid.uuid4().hex
    mac = "02:fe:" + ":".join(marker[i:i+2] for i in range(0, 8, 2))
    guid = "smoke-" + marker
    secret = subprocess.check_output(["docker", "exec", "xiaozhi-esp32-server", "python", "-c",
        "import yaml; print(yaml.safe_load(open('data/.config.yaml'))['manager-api']['secret'])"], text=True).strip()
    checks = 0
    try:
        source_agent = database("SELECT a.id FROM ai_agent a JOIN ai_device d ON d.agent_id=a.id "
            "WHERE d.mac_address NOT LIKE '02:fe:%' ORDER BY d.last_connected_at DESC LIMIT 1")
        assert source_agent
        for user_id, token in zip((owner, other), tokens):
            database(f"INSERT INTO sys_user(id,username,password,super_admin,status,create_date,update_date) VALUES "
                f"({user_id},{sql('smoke-'+str(user_id))},NULL,0,1,NOW(),NOW()); "
                f"INSERT INTO sys_user_token(id,user_id,token,expire_date,update_date,create_date) VALUES "
                f"({user_id},{user_id},{sql(token)},DATE_ADD(NOW(),INTERVAL 15 MINUTE),NOW(),NOW())")
        clone("ai_agent", source_agent, agent_id, {"user_id": owner,
            "agent_code": marker, "agent_name": "Binding API Smoke Fixture", "summary_memory": None})
        database("INSERT INTO ai_device(id,user_id,mac_address,agent_id,alias,auto_update,board,app_version) VALUES (" +
            ",".join(map(sql, (device_id, owner, mac, agent_id, "Binding smoke fixture", 0, "test-fixture", "test"))) + ")")
        profile = {"nickname": "Smoke Alice", "language": "zh-CN", "interests": "history", "explanationStyle": "brief"}
        api("PUT", "/user/profile/" + guid, tokens[0], profile)
        assert api("GET", "/user/profile/" + guid, tokens[1]) is None
        path = "/device/" + mac + "/user-binding"
        api("GET", path, tokens[1], ok=False)
        checks += 2
        binding = api("POST", path, tokens[0], {"bootID": "boot-" + marker, "userGUID": guid})
        command = binding["command"]
        assert type(command["bindingVersion"]) is int, "bindingVersion must be a JSON integer, got " + type(command["bindingVersion"]).__name__
        assert command["bindingToken"] and command["cmd"] == "bindUser"
        resolve = {key: command[key] for key in ("bootID", "userGUID", "bindingVersion", "bindingToken")}
        resolve["macAddress"] = mac
        assert api("POST", "/config/user-binding/resolve", secret, resolve)["usageSessionID"] == binding["usageSessionID"]
        current = api("GET", path, tokens[0])
        assert "bindingToken" not in current
        api("POST", "/config/user-binding/resolve", secret, dict(resolve, bootID="old-boot"), ok=False)
        api("POST", "/config/user-binding/resolve", secret, dict(resolve, bindingToken="invalid-token-" * 5), ok=False)
        checks += 5
        api("PUT", "/user/profile/" + guid, tokens[0], dict(profile, nickname="Changed for next binding"))
        assert api("POST", "/config/user-binding/resolve", secret, resolve)["profile"]["nickname"] == "Smoke Alice"
        now = int(time.time() * 1000)
        report = {"macAddress": mac, "userGuid": guid, "sessionId": str(uuid.uuid4()),
            "usageSessionId": binding["usageSessionID"], "bootId": command["bootID"],
            "bindingVersion": command["bindingVersion"], "roleId": "smoke-role", "turnId": "smoke-turn",
            "messageId": "smoke-" + uuid.uuid4().hex, "deliveryStatus": "generated", "chatType": 2,
            "content": "Hello", "reportTime": now, "audioBase64": None, "textTruncated": False}
        assert api("POST", "/agent/chat-history/report", secret, report) is True
        assert api("POST", "/agent/chat-history/report", secret, report) is True
        api("POST", "/agent/chat-history/report", secret, dict(report, userGuid="wrong-user"), ok=False)
        api("POST", "/agent/chat-history/report", secret, dict(report, content="conflict"), ok=False)
        second = dict(report, messageId="smoke-" + uuid.uuid4().hex, content=" ", reportTime=now-1)
        assert api("POST", "/agent/chat-history/report", secret, second) is True
        history = "/user/profile/" + guid + "/sessions/" + binding["usageSessionID"] + "/messages"
        rows = api("GET", history, tokens[0])
        assert len(rows) == 2 and rows[0]["content"] == " " and rows[1]["content"] == "Hello"
        api("GET", history, tokens[1], ok=False)
        checks += 8
        unbind = api("DELETE", path, tokens[0])
        assert unbind["cmd"] == "unbindUser" and unbind["bindingVersion"] > command["bindingVersion"]
        api("POST", "/config/user-binding/resolve", secret, resolve, ok=False)
        delayed = dict(report, messageId="smoke-" + uuid.uuid4().hex, content="Delayed original user", reportTime=now+2)
        assert api("POST", "/agent/chat-history/report", secret, delayed) is True
        rebound = api("POST", path, tokens[0], {"bootID": command["bootID"], "userGUID": "next-"+guid})
        assert rebound["bindingVersion"] > unbind["bindingVersion"]
        assert rebound["usageSessionID"] != binding["usageSessionID"]
        assert len(api("GET", history, tokens[0])) == 3
        checks += 6
        if os.environ.get("SMOKE_WEBSOCKET") == "1":
            websocket_smoke(mac, rebound)
            new_history = "/user/profile/next-" + guid + "/sessions/" + rebound["usageSessionID"] + "/messages"
            rows = []
            for _ in range(30):
                rows = api("GET", new_history, tokens[0], None)
                if any(row["chatType"] == 1 for row in rows) and any(row["chatType"] == 2 for row in rows):
                    break
                time.sleep(1)
            assert rows and all(row["userGuid"] == "next-" + guid for row in rows)
            assert any(row["chatType"] == 1 for row in rows) and any(row["chatType"] == 2 for row in rows)
            print("WEBSOCKET_DURABLE_HISTORY_OK records=" + str(len(rows)))
        with ThreadPoolExecutor(max_workers=2) as pool:
            requests = [pool.submit(api, "POST", path, tokens[0],
                {"bootID": command["bootID"], "userGUID": "parallel-" + str(i) + "-" + marker}) for i in range(2)]
            concurrent = [request.result() for request in requests]
        versions = sorted(item["bindingVersion"] for item in concurrent)
        assert versions[1] == versions[0] + 1
        assert api("GET", path, tokens[0])["bindingVersion"] == versions[1]
        checks += 2
        print(json.dumps({"api_checks_passed": checks, "real_http": True, "real_mysql": True,
                          "production_devices_modified": False}))
    finally:
        # A synthetic session must not leave retry jobs referencing deleted fixtures.
        cleanup_outbox = r'''
import sqlite3, sys, time
mac=sys.stdin.read().strip()
db=sqlite3.connect('data/report-outbox.sqlite3',timeout=5)
for _ in range(30):
    n=db.execute("SELECT COUNT(*) FROM report_outbox WHERE json_extract(payload,'$.macAddress')=? AND status='pending'",(mac,)).fetchone()[0]
    if not n: break
    time.sleep(1)
db.execute("DELETE FROM report_outbox WHERE json_extract(payload,'$.macAddress')=?",(mac,))
db.commit()
'''
        subprocess.run(["docker", "exec", "-i", "xiaozhi-esp32-server", "python", "-c", cleanup_outbox],
            input=mac, text=True, capture_output=True, timeout=40)
        database(f"DELETE FROM ai_agent_chat_history WHERE owner_id={owner}; "
            f"DELETE FROM ai_usage_session WHERE owner_id={owner}; "
            f"DELETE FROM ai_device_binding_state WHERE mac_address={sql(mac)}; "
            f"DELETE FROM ai_user_profile WHERE owner_id IN ({owner},{other}); "
            f"DELETE FROM ai_device WHERE id={sql(device_id)}; DELETE FROM ai_agent WHERE id={sql(agent_id)}; "
            f"DELETE FROM sys_user_token WHERE user_id IN ({owner},{other}); "
            f"DELETE FROM sys_user WHERE id IN ({owner},{other})")
        print("SMOKE_FIXTURES_REMOVED")


if __name__ == "__main__":
    main()
