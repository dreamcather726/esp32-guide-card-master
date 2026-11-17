"""Live API/DB user-switch checks on disposable fixtures; never publishes MQTT."""
import json
import secrets
import subprocess
import time
import uuid

from smoke_api import api, clone, database, sql


def main():
    marker = uuid.uuid4().hex
    owner = int(time.time() * 1000000)
    token = secrets.token_hex(32)
    agent = uuid.uuid4().hex
    devices = [uuid.uuid4().hex for _ in range(2)]
    macs = ["02:fc:" + ":".join(marker[i:i + 2] for i in range(start, start + 8, 2))
            for start in (0, 8)]
    users = ["switch-a-" + marker, "switch-b-" + marker]
    cabs = ["switch-cab-" + marker + str(i) for i in range(2)]
    boots = ["switch-boot-" + marker + str(i) for i in range(2)]
    secret = subprocess.check_output([
        "docker", "exec", "xiaozhi-esp32-server", "python", "-c",
        "import yaml; print(yaml.safe_load(open('data/.config.yaml'))['manager-api']['secret'])"
    ], text=True).strip()
    checked = []
    assert database("SELECT COUNT(*) FROM sys_user WHERE id=" + str(owner)) == "0"
    for mac in macs:
        assert database("SELECT COUNT(*) FROM ai_device WHERE mac_address=" + sql(mac)) == "0"

    def resolve(index, binding, ok=True):
        body = {key: binding[key] for key in ("bootID", "userGUID", "bindingVersion", "bindingToken")}
        return api("POST", "/config/user-binding/resolve", secret,
                   dict(body, macAddress=macs[index]), ok=ok)

    def legacy(index, user):
        request = {"cabGuid": cabs[index], "bootID": boots[index], "userGUID": user,
                   "requestID": str(uuid.uuid4())}
        return api("POST", "/config/user-binding/legacy", secret, request), request

    def report(index, binding, content):
        body = {"macAddress": macs[index], "userGuid": binding["userGUID"],
                "sessionId": str(uuid.uuid4()), "usageSessionId": binding["usageSessionID"],
                "bootId": binding["bootID"], "bindingVersion": binding["bindingVersion"],
                "messageId": "switch-" + uuid.uuid4().hex, "chatType": 2,
                "content": content, "deliveryStatus": "generated",
                "reportTime": int(time.time() * 1000), "textTruncated": False}
        assert api("POST", "/agent/chat-history/report", secret, body) is True

    def history(binding):
        return api("GET", "/user/profile/" + binding["userGUID"] + "/sessions/" +
                   binding["usageSessionID"] + "/messages", token)

    try:
        source = database("SELECT a.id FROM ai_agent a JOIN ai_device d ON d.agent_id=a.id "
                          "WHERE d.mac_address NOT LIKE '02:%' ORDER BY d.last_connected_at DESC LIMIT 1")
        assert source
        database(f"INSERT INTO sys_user(id,username,password,super_admin,status,create_date,update_date) VALUES "
                 f"({owner},{sql('switch-smoke-' + marker)},NULL,0,1,NOW(),NOW()); "
                 f"INSERT INTO sys_user_token(id,user_id,token,expire_date,update_date,create_date) VALUES "
                 f"({owner},{owner},{sql(token)},DATE_ADD(NOW(),INTERVAL 15 MINUTE),NOW(),NOW())")
        clone("ai_agent", source, agent, {"user_id": owner, "agent_code": marker,
              "agent_name": "User Switch Smoke Fixture", "summary_memory": None})
        for index, mac in enumerate(macs):
            database("INSERT INTO ai_device(id,user_id,mac_address,agent_id,alias,auto_update,board,app_version) VALUES (" +
                     ",".join(map(sql, (devices[index], owner, mac, agent, "User switch fixture", 0,
                                        "test-fixture", "test"))) + ")")
            api("PUT", "/device/" + mac + "/mqtt-binding", token, {"cabGuid": cabs[index]})
            api("POST", "/config/user-binding/online", secret,
                {"cmd": "KJ", "cabGuid": cabs[index], "macAddress": mac, "bootID": boots[index],
                 "userGUID": "", "bindingState": "unbound", "bindingVersion": 0, "usageSessionID": ""})
        for user, nickname in zip(users, ("User A", "User B")):
            api("PUT", "/user/profile/" + user, token,
                {"nickname": nickname, "language": "zh-CN", "interests": "history",
                 "explanationStyle": "brief"})

        first = api("POST", "/device/" + macs[0] + "/user-binding", token,
                    {"bootID": boots[0], "userGUID": users[0]})
        assert resolve(0, first)["profile"]["nickname"] == "User A"
        report(0, first, "A before switch")
        checked.append("manual API binding loads user A profile")

        other_device, _ = legacy(1, users[0])
        assert resolve(1, other_device)["profile"]["nickname"] == "User A"
        assert resolve(0, first)["usageSessionID"] == first["usageSessionID"]
        assert first["usageSessionID"] != other_device["usageSessionID"]
        resolve(0, other_device, ok=False)
        checked.append("same user works on another MAC with independent device credentials")

        second, second_request = legacy(0, users[1])
        assert second["bindingVersion"] > first["bindingVersion"]
        assert second["usageSessionID"] != first["usageSessionID"]
        assert resolve(0, second)["profile"]["nickname"] == "User B"
        resolve(0, first, ok=False)
        assert resolve(1, other_device)["userGUID"] == users[0]
        checked.append("same MAC changes A to B without affecting another device")

        report(0, second, "B after switch")
        report(0, first, "A delayed original report")
        assert {row["content"] for row in history(first)} == {"A before switch", "A delayed original report"}
        assert [row["content"] for row in history(second)] == ["B after switch"]
        api("GET", "/user/profile/" + users[1] + "/sessions/" + first["usageSessionID"] +
            "/messages", token, ok=False)
        checked.append("old and delayed transcripts remain with A; B history isolated")

        third, _ = legacy(0, users[0])
        assert third["bindingVersion"] > second["bindingVersion"]
        assert len({item["usageSessionID"] for item in (first, second, third)}) == 3
        assert resolve(0, third)["profile"]["nickname"] == "User A"
        resolve(0, second, ok=False)
        api("POST", "/config/user-binding/legacy", secret, second_request, ok=False)
        assert api("GET", "/device/" + macs[0] + "/user-binding", token)["usageSessionID"] == third["usageSessionID"]
        assert len(history(first)) == 2 and not history(third)
        checked.append("B to A creates a fresh session; old request cannot switch back to B")

        unbound = api("DELETE", "/device/" + macs[0] + "/user-binding", token)
        assert unbound["bindingVersion"] > third["bindingVersion"]
        resolve(0, third, ok=False)
        assert api("GET", "/device/" + macs[0] + "/user-binding", token) is None
        assert resolve(1, other_device)["userGUID"] == users[0]
        assert api("GET", "/user/profile/" + users[0], token)["profile"]["nickname"] == "User A"
        assert len(history(first)) == 2 and len(history(second)) == 1
        checked.append("unbind preserves profiles/history and other device session")
        print(json.dumps({"result": "passed", "scenarios": checked, "real_http_mysql": True,
                          "production_device_identities_modified": False, "mqtt_published": False}))
    finally:
        owned_macs = "SELECT mac_address FROM ai_device WHERE user_id=" + str(owner) + " AND id IN (" + ",".join(map(sql, devices)) + ")"
        database("DELETE FROM ai_mqtt_binding_outbox WHERE mac_address IN (" + owned_macs + "); "
                 "DELETE FROM ai_mqtt_binding_mapping WHERE mac_address IN (" + owned_macs + "); "
                 f"DELETE FROM ai_agent_chat_history WHERE owner_id={owner}; "
                 f"DELETE FROM ai_usage_session WHERE owner_id={owner}; "
                 "DELETE FROM ai_device_binding_state WHERE mac_address IN (" + owned_macs + "); "
                 f"DELETE FROM ai_user_profile WHERE owner_id={owner}; "
                 "DELETE FROM ai_device WHERE id IN (" + ",".join(map(sql, devices)) + "); "
                 "DELETE FROM ai_agent WHERE id=" + sql(agent) + "; "
                 f"DELETE FROM sys_user_token WHERE user_id={owner}; "
                 f"DELETE FROM sys_user WHERE id={owner}")
        assert database("SELECT COUNT(*) FROM ai_device WHERE id IN (" +
                        ",".join(map(sql, devices)) + ")") == "0"
        print("USER_SWITCH_FIXTURES_REMOVED")


if __name__ == "__main__":
    main()
