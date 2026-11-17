#ifndef USER_BINDING_JSON_H
#define USER_BINDING_JSON_H

#include "user_binding.h"
#include <cJSON.h>
#include <cmath>
#include <cstring>
#include <initializer_list>

inline std::string BindingJsonString(const cJSON* root, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

inline uint64_t BindingJsonVersion(const cJSON* root) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, "bindingVersion");
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < 1 || item->valuedouble > kMaxBindingVersion ||
        std::floor(item->valuedouble) != item->valuedouble) return 0;
    return static_cast<uint64_t>(item->valuedouble);
}

inline UserBindingCommand ParseUserBindingCommand(const cJSON* root) {
    UserBindingCommand command;
    const auto action = BindingJsonString(root, "cmd");
    command.valid_action = action == "bindUser" || action == "unbindUser";
    for (const char* key : {"cmd", "cmdID", "bootID", "userGUID", "bindingToken", "usageSessionID"}) {
        const auto* field = cJSON_GetObjectItemCaseSensitive(root, key);
        if (field && !cJSON_IsString(field)) command.valid_action = false;
    }
    command.command_id = BindingJsonString(root, "cmdID");
    command.unbind = BindingJsonString(root, "cmd") == "unbindUser";
    command.binding.boot_id = BindingJsonString(root, "bootID");
    command.binding.version = BindingJsonVersion(root);
    command.binding.user_guid = BindingJsonString(root, "userGUID");
    command.binding.token = BindingJsonString(root, "bindingToken");
    command.binding.usage_session_id = BindingJsonString(root, "usageSessionID");
    return command;
}

inline bool IsLegacyUserBindingRequest(const cJSON* root) {
    for (const char* key : {"cmd", "bootID", "bindingVersion", "bindingToken", "usageSessionID"}) {
        if (cJSON_HasObjectItem(root, key)) return false;
    }
    for (const char* key : {"userGUID", "userGuid", "user_guid"}) {
        const auto* user = cJSON_GetObjectItemCaseSensitive(root, key);
        if (user) {
            return cJSON_IsString(user) && user->valuestring &&
                   IsBindingValue(user->valuestring);
        }
    }
    return false;
}

// Visit every child, including duplicate keys and nested arrays. Failure never
// permits a caller to log the original JSON as a fallback.
inline bool RedactSensitiveJsonFields(cJSON* item) {
    if (!item) return true;
    for (cJSON* child = item->child; child;) {
        cJSON* next = child->next;
        bool sensitive = false;
        if (child->string) {
            for (const char* key : {"WIFIPW", "WIFI PW", "wifiPW", "wifi_pw", "bindingToken"}) {
                if (std::strcmp(child->string, key) == 0) sensitive = true;
            }
        }
        if (sensitive) {
            cJSON* replacement = cJSON_CreateString("***");
            if (!replacement) return false;
            const size_t key_size = std::strlen(child->string) + 1;
            replacement->string = static_cast<char*>(cJSON_malloc(key_size));
            if (!replacement->string) {
                cJSON_Delete(replacement);
                return false;
            }
            std::memcpy(replacement->string, child->string, key_size);
            if (!cJSON_ReplaceItemViaPointer(item, child, replacement)) {
                cJSON_Delete(replacement);
                return false;
            }
        } else if (!RedactSensitiveJsonFields(child)) {
            return false;
        }
        child = next;
    }
    return true;
}

inline bool IsReadyUserContext(const cJSON* root, const UserBindingSnapshot& binding) {
    return binding.IsBound() && BindingJsonString(root, "state") == "ready" &&
           BindingJsonString(root, "bootID") == binding.boot_id &&
           BindingJsonString(root, "userGUID") == binding.user_guid &&
           BindingJsonVersion(root) == binding.version &&
           BindingJsonString(root, "usageSessionID") == binding.usage_session_id;
}

#endif
