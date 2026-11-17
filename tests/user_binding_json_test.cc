#ifdef NDEBUG
#undef NDEBUG
#endif
#include "user_binding_json.h"
#include <cassert>
#include <iostream>
#include <memory>

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
static Json Parse(const char* text) { return Json(cJSON_Parse(text), cJSON_Delete); }

int main() {
    const auto payload = Parse(R"({"cmd":"bindUser","cmdID":"bind-1","bootID":"boot-A",
        "userGUID":"user-A","bindingVersion":1,"bindingToken":"opaque","usageSessionID":"usage-A"})");
    const auto command = ParseUserBindingCommand(payload.get());
    assert(!IsLegacyUserBindingRequest(payload.get()));
    BootUserBinding store("boot-A");
    std::string error;
    assert(store.Apply(command, error) == BindingCommandResult::Accepted);
    for (const char* invalid : {"0", "-1", "1.5", "\"1\"", "null", "true", "1e100", "9007199254740992"}) {
        auto root = Parse((std::string("{\"bindingVersion\":") + invalid + "}").c_str());
        assert(BindingJsonVersion(root.get()) == 0);
    }
    auto maximum = Parse("{\"bindingVersion\":9007199254740991}");
    assert(BindingJsonVersion(maximum.get()) == kMaxBindingVersion);

    auto ready = Parse(R"({"type":"user_context","state":"ready","status":"ready",
        "userGUID":"user-A","bootID":"boot-A","bindingVersion":1,"usageSessionID":"usage-A"})");
    assert(IsReadyUserContext(ready.get(), command.binding));
    for (const char* key : {"state", "userGUID", "bootID", "usageSessionID", "bindingVersion"}) {
        Json mismatched(cJSON_Duplicate(ready.get(), true), cJSON_Delete);
        cJSON_ReplaceItemInObjectCaseSensitive(mismatched.get(), key, cJSON_CreateString("wrong"));
        assert(!IsReadyUserContext(mismatched.get(), command.binding));
    }
    auto status_only = Parse(R"({"status":"ready","usageSessionID":"usage-A"})");
    assert(!IsReadyUserContext(status_only.get(), command.binding));
    auto failure = Parse(R"({"type":"user_context","state":"error","status":"error","code":"binding_invalid"})");
    assert(!IsReadyUserContext(failure.get(), command.binding));

    auto unbind = Parse(R"({"cmd":"unbindUser","cmdID":"unbind-2","bootID":"boot-A","bindingVersion":2,"userGUID":""})");
    assert(!IsLegacyUserBindingRequest(unbind.get()));
    assert(store.Apply(ParseUserBindingCommand(unbind.get()), error) == BindingCommandResult::Accepted);
    auto bad_unbind = Parse(R"({"cmd":"unbindUser","cmdID":"unbind-3","bootID":"boot-A","bindingVersion":3,"userGUID":123})");
    assert(store.Apply(ParseUserBindingCommand(bad_unbind.get()), error) == BindingCommandResult::Rejected);
    auto legacy = Parse(R"({"cmdID":"legacy","bootID":"boot-A","bindingVersion":3,"userGUID":"legacy-user"})");
    assert(!IsLegacyUserBindingRequest(legacy.get()));
    assert(store.Apply(ParseUserBindingCommand(legacy.get()), error) == BindingCommandResult::Rejected);

    for (const char* request : {
            R"({"groupID":"9","userGUID":"DTkxHempvRmp"})",
            R"({"userGUID":"DTkxHempvRmp"})",
            R"({"cmdID":"legacy-request","groupId":9,"userGuid":"user-A"})",
            R"({"group_id":"9","user_guid":"user-A"})"}) {
        auto root = Parse(request);
        assert(IsLegacyUserBindingRequest(root.get()));
        const auto before = store.Get();
        assert(store.Apply(ParseUserBindingCommand(root.get()), error) == BindingCommandResult::Rejected);
        assert(store.Get().SameIdentity(before) && store.Get().state == before.state);
    }
    for (const char* request : {
            R"({"groupID":"9"})", R"({"userGUID":""})", R"({"userGUID":null})",
            R"({"userGUID":123})", R"({"userGUID":true})", R"({"userGUID":{}})",
            R"({"userGUID":"user with spaces"})", R"({"userGUID":"user\r\nA"})",
            R"({"cmd":"bindUser","userGUID":"user-A"})",
            R"({"cmd":"unbindUser","userGUID":"user-A"})",
            R"({"cmd":"bindingAuto","userGUID":"user-A"})",
            R"({"cmd":"other","userGUID":"user-A"})",
            R"({"cmd":null,"userGUID":"user-A"})",
            R"({"cmd":"","userGUID":"user-A"})",
            R"({"bootID":"boot-A","userGUID":"user-A"})",
            R"({"bindingVersion":1,"userGUID":"user-A"})",
            R"({"bindingToken":"opaque","userGUID":"user-A"})",
            R"({"usageSessionID":"usage-A","userGUID":"user-A"})"}) {
        auto root = Parse(request);
        assert(!IsLegacyUserBindingRequest(root.get()));
    }
    auto wrapped_legacy = Parse(R"({"payload":{"groupID":"9","userGUID":"DTkxHempvRmp"}})");
    assert(IsLegacyUserBindingRequest(cJSON_GetObjectItemCaseSensitive(wrapped_legacy.get(), "payload")));

    auto secrets = Parse(R"({"bindingToken":"secret-one","bindingToken":"secret-two",
        "payload":[{"bindingToken":{"value":"secret-three"},"WIFIPW":"password"}],"cmd":"bindUser"})");
    assert(RedactSensitiveJsonFields(secrets.get()));
    char* serialized = cJSON_PrintUnformatted(secrets.get());
    assert(serialized);
    const std::string log(serialized);
    cJSON_free(serialized);
    assert(log.find("secret-") == std::string::npos);
    assert(log.find("password") == std::string::npos);
    assert(log.find("bindUser") != std::string::npos);
    std::cout << "binding JSON and redaction tests passed\n";
}
