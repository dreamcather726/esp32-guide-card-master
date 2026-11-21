#ifndef USER_BINDING_H
#define USER_BINDING_H

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

constexpr uint64_t kMaxBindingVersion = 9007199254740991ULL;

enum class BindingState { Unbound, Pending, Ready, Error };
enum class BindingCommandResult { Rejected, Duplicate, Accepted };

inline const char* BindingStateName(BindingState state) {
    switch (state) {
        case BindingState::Pending: return "pending";
        case BindingState::Ready: return "ready";
        case BindingState::Error: return "error";
        default: return "unbound";
    }
}

// These values also travel in HTTP headers; never accept control characters.
inline bool IsBindingValue(const std::string& value, size_t max_length = 256) {
    if (value.empty() || value.size() > max_length) return false;
    for (unsigned char ch : value) {
        if (ch <= 0x20 || ch > 0x7e) return false;
    }
    return true;
}

struct UserBindingSnapshot {
    std::string boot_id;
    std::string user_guid;
    uint64_t version = 0;
    std::string token;
    std::string usage_session_id;
    BindingState state = BindingState::Unbound;

    bool IsBound() const {
        return version > 0 && !user_guid.empty() && !token.empty() &&
               !usage_session_id.empty();
    }

    bool SameIdentity(const UserBindingSnapshot& other) const {
        return boot_id == other.boot_id && version == other.version &&
               user_guid == other.user_guid && token == other.token &&
               usage_session_id == other.usage_session_id;
    }
};

struct UserBindingCommand {
    UserBindingSnapshot binding;
    std::string command_id;
    bool unbind = false;
    bool valid_action = true;
};

// RAM only. The version watermark survives unbind/reconnect, never an ESP boot.
class BootUserBinding {
public:
    explicit BootUserBinding(std::string boot_id) {
        binding_.boot_id = std::move(boot_id);
    }

    UserBindingSnapshot Get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return binding_;
    }

    BindingCommandResult Check(const UserBindingCommand& command,
                               std::string& error) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return CheckLocked(command, error);
    }

    BindingCommandResult Apply(const UserBindingCommand& command,
                               std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto result = CheckLocked(command, error);
        if (result != BindingCommandResult::Accepted) return result;
        binding_ = command.binding;
        binding_.state = command.unbind ? BindingState::Unbound : BindingState::Pending;
        last_command_id_ = command.command_id;
        return result;
    }

    bool SetState(const UserBindingSnapshot& expected, BindingState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!binding_.IsBound() || !binding_.SameIdentity(expected) ||
            state == BindingState::Unbound) return false;
        binding_.state = state;
        return true;
    }

private:
    mutable std::mutex mutex_;
    UserBindingSnapshot binding_;
    std::string last_command_id_;

    BindingCommandResult CheckLocked(const UserBindingCommand& command,
                                     std::string& error) const {
        error.clear();
        const auto& next = command.binding;
        if (!command.valid_action) {
            error = "expected bindUser or unbindUser command with string identity fields";
        } else if (next.boot_id != binding_.boot_id || !IsBindingValue(command.command_id) ||
            next.version == 0 || next.version > kMaxBindingVersion) {
            error = "matching bootID, nonempty cmdID and positive integer bindingVersion required";
        } else if (!command.unbind &&
                   (!IsBindingValue(next.user_guid) || !IsBindingValue(next.token, 4096) ||
                    !IsBindingValue(next.usage_session_id))) {
            error = "userGUID, bindingToken and usageSessionID required";
        } else if (command.unbind &&
                   (!next.user_guid.empty() || !next.token.empty() ||
                    !next.usage_session_id.empty())) {
            error = "unbindUser must omit identity credentials or use empty strings";
        } else if (next.version == binding_.version &&
                   command.command_id == last_command_id_ &&
                   next.SameIdentity(binding_)) {
            return BindingCommandResult::Duplicate;
        } else if (next.version <= binding_.version || command.command_id == last_command_id_) {
            error = "stale/conflicting bindingVersion or reused cmdID";
        } else {
            return BindingCommandResult::Accepted;
        }
        return BindingCommandResult::Rejected;
    }
};

#endif
