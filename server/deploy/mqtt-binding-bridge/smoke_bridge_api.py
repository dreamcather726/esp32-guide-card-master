"""Real HTTP/MySQL checks on disposable devices, never sends MQTT commands."""
import secrets
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from smoke_api import api, clone, database, sql
from host_admin import secret


def main():
    marker = uuid.uuid4().hex
    owner, other = int(time.time() * 1000000), int(time.time() * 1000000) + 1
    tokens = [secrets.token_hex(32), secrets.token_hex(32)]
    agent, device = uuid.uuid4().hex, uuid.uuid4().hex
    mac = "02:fd:" + ":".join(marker[i:i+2] for i in range(0, 8, 2))
    cab, guid, boot = "fixture-" + marker, "bridge-user-" + marker, "boot-" + marker
    service_secret = secret()
    prefix = "/device/" + mac
    try:
        source = database("SELECT a.id FROM ai_agent a JOIN ai_device d ON d.agent_id=a.id "
                          "WHERE d.mac_address NOT LIKE '02:%' ORDER BY d.last_connected_at DESC LIMIT 1")
        assert source
        for user_id, token in zip((owner, other), tokens):
            database(f"INSERT INTO sys_user(id,username,password,super_admin,status,create_date,update_date) VALUES "
                     f"({user_id},{sql('bridge-smoke-'+str(user_id))},NULL,0,1,NOW(),NOW()); "
                     f"INSERT INTO sys_user_token(id,user_id,token,expire_date,update_date,create_date) VALUES "
                     f"({user_id},{user_id},{sql(token)},DATE_ADD(NOW(),INTERVAL 15 MINUTE),NOW(),NOW())")
        clone("ai_agent", source, agent, {"user_id": owner, "agent_code": marker,
              "agent_name": "MQTT Bridge Test Fixture", "summary_memory": None})
        database("INSERT INTO ai_device(id,user_id,mac_address,agent_id,alias,auto_update,board,app_version) VALUES (" +
                 ",".join(map(sql, (device, owner, mac, agent, "MQTT bridge smoke fixture", 0, "test-fixture", "test"))) + ")")
        mapping = prefix + "/mqtt-binding"
        bind = prefix + "/user-binding"
        online = {"cmd": "KJ", "cabGuid": cab, "macAddress": mac, "bootID": boot, "groupID": "9",
                  "userGUID": "", "bindingState": "unbound", "bindingVersion": 0, "usageSessionID": "",
                  "bindingToken": "redact-me", "password": "redact-me"}
        request = {"cabGuid": cab, "bootID": boot, "userGUID": guid, "requestID": str(uuid.uuid4())}
        api("POST", "/config/user-binding/online", service_secret, online, ok=False)
        api("PUT", mapping, tokens[1], {"cabGuid": cab}, ok=False)
        assert api("PUT", mapping, tokens[0], {"cabGuid": cab})["cabGuid"] == cab
        api("GET", mapping, tokens[1], ok=False)
        api("POST", "/config/user-binding/legacy", service_secret, request, ok=False)
        api("POST", "/config/user-binding/online", tokens[0], online, ok=False)
        api("POST", "/config/user-binding/online", service_secret, dict(online, macAddress="02:01:02:03:04:05"), ok=False)
        api("POST", "/config/user-binding/online", service_secret, online)
        view = api("GET", mapping, tokens[0])
        assert view["fresh"] is True and view["report"]["bootID"] == boot
        assert "bindingToken" not in view["report"] and "password" not in view["report"]
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(api, "POST", "/config/user-binding/legacy", service_secret, request) for _ in range(2)]
            first, repeat = [future.result() for future in futures]
        assert first["command"] == repeat["command"]
        assert type(first["bindingVersion"]) is int
        same_user = api("POST", "/config/user-binding/legacy", service_secret, dict(request, requestID=str(uuid.uuid4())))
        assert first["command"] == same_user["command"]
        command = first["command"]
        resolve = {key: command[key] for key in ("userGUID", "bootID", "bindingVersion", "bindingToken")}
        resolve["macAddress"] = mac
        api("POST", "/config/user-binding/resolve", service_secret, resolve)
        current = api("GET", bind, tokens[0])
        assert "bindingToken" not in current and "command" not in current
        encrypted = database("SELECT encrypted_binding FROM ai_mqtt_binding_outbox WHERE mac_address=" + sql(mac))
        assert command["bindingToken"] not in encrypted and 'bindingToken' not in encrypted
        api("POST", "/config/user-binding/legacy", service_secret, dict(request, userGUID="different"), ok=False)
        api("POST", "/config/user-binding/legacy", service_secret, dict(request, bootID="old-boot"), ok=False)
        database("UPDATE ai_mqtt_binding_outbox SET expires_at=DATE_SUB(NOW(),INTERVAL 1 SECOND) WHERE mac_address=" + sql(mac))
        api("POST", "/config/user-binding/legacy", service_secret, request, ok=False)
        recovered = api("POST", "/config/user-binding/legacy", service_secret, dict(request, requestID=str(uuid.uuid4())))
        assert recovered["bindingVersion"] > first["bindingVersion"]
        api("DELETE", bind, tokens[0])
        api("POST", "/config/user-binding/legacy", service_secret, request, ok=False)
        api("POST", "/config/user-binding/resolve", service_secret, resolve, ok=False)
        api("POST", "/config/user-binding/online", service_secret, dict(online, bootID="new-boot"))
        api("POST", "/config/user-binding/legacy", service_secret, dict(request, requestID=str(uuid.uuid4())), ok=False)
        new_boot = api("POST", "/config/user-binding/legacy", service_secret,
                       dict(request, bootID="new-boot", requestID=str(uuid.uuid4())))
        assert new_boot["bootID"] == "new-boot"
        database("UPDATE ai_mqtt_binding_mapping SET received_at=DATE_SUB(NOW(),INTERVAL 181 SECOND) WHERE mac_address=" + sql(mac))
        assert api("GET", mapping, tokens[0])["fresh"] is False
        api("POST", "/config/user-binding/legacy", service_secret,
            dict(request, bootID="new-boot", requestID=str(uuid.uuid4())), ok=False)
        database("UPDATE ai_device SET user_id=" + str(other) + " WHERE id=" + sql(device))
        api("POST", "/config/user-binding/online", service_secret, online, ok=False)
        assert api("GET", mapping, tokens[1])["cabGuid"] is None
        print("MQTT_BINDING_REAL_API_DB_CHECKS_OK")
    finally:
        database("DELETE FROM ai_mqtt_binding_outbox WHERE mac_address=" + sql(mac) + "; "
                 "DELETE FROM ai_mqtt_binding_mapping WHERE mac_address=" + sql(mac) + "; "
                 "DELETE FROM ai_usage_session WHERE mac_address=" + sql(mac) + "; "
                 "DELETE FROM ai_device_binding_state WHERE mac_address=" + sql(mac) + "; "
                 "DELETE FROM ai_user_profile WHERE owner_id IN (" + str(owner) + "," + str(other) + "); "
                 "DELETE FROM ai_device WHERE id=" + sql(device) + "; "
                 "DELETE FROM ai_agent WHERE id=" + sql(agent) + "; "
                 f"DELETE FROM sys_user_token WHERE user_id IN ({owner},{other}); "
                 f"DELETE FROM sys_user WHERE id IN ({owner},{other})")
        print("MQTT_BINDING_FIXTURES_CLEANED")


if __name__ == "__main__":
    main()
