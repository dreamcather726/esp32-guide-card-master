#ifdef NDEBUG
#undef NDEBUG
#endif
#include "user_binding.h"
#include <cassert>
#include <iostream>

static UserBindingCommand Bind(uint64_t version = 1) {
    UserBindingCommand command;
    command.command_id = "command-" + std::to_string(version);
    command.binding.boot_id = "boot-A";
    command.binding.user_guid = "user-A";
    command.binding.version = version;
    command.binding.token = "opaque-token";
    command.binding.usage_session_id = "usage-A";
    return command;
}

int main() {
    BootUserBinding store("boot-A");
    std::string error;
    assert(store.Get().state == BindingState::Unbound);
    assert(store.Get().version == 0);
    assert(!store.Get().IsBound());

    auto invalid = Bind();
    invalid.binding.boot_id = "previous-boot";
    assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);
    invalid = Bind();
    invalid.command_id.clear();
    assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);
    invalid = Bind(0);
    assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);
    invalid = Bind(kMaxBindingVersion + 1);
    assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);
    invalid = Bind();
    invalid.valid_action = false;
    assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);
    for (int field = 0; field < 3; ++field) {
        invalid = Bind();
        if (field == 0) invalid.binding.user_guid.clear();
        if (field == 1) invalid.binding.token.clear();
        if (field == 2) invalid.binding.usage_session_id.clear();
        assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);
    }
    invalid = Bind();
    invalid.binding.token = "secret\r\nInjected:1";
    assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);
    assert(!store.Get().IsBound());

    const auto first = Bind();
    assert(store.Apply(first, error) == BindingCommandResult::Accepted);
    assert(store.Get().state == BindingState::Pending);
    assert(store.Apply(first, error) == BindingCommandResult::Duplicate);
    assert(store.SetState(first.binding, BindingState::Ready));
    assert(store.Apply(first, error) == BindingCommandResult::Duplicate);
    assert(store.Get().state == BindingState::Ready);
    invalid = first;
    invalid.binding.token = "different-token";
    assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);
    invalid = first;
    invalid.command_id = "different-command";
    assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);

    auto second = Bind(2);
    second.binding.user_guid = "user-B";
    second.binding.usage_session_id = "usage-B";
    assert(store.Apply(second, error) == BindingCommandResult::Accepted);
    assert(!store.SetState(first.binding, BindingState::Ready));
    assert(store.Get().state == BindingState::Pending);
    assert(store.SetState(second.binding, BindingState::Error));
    assert(store.Apply(second, error) == BindingCommandResult::Duplicate);
    assert(store.Get().state == BindingState::Error);

    UserBindingCommand unbind;
    unbind.command_id = "unbind-3";
    unbind.unbind = true;
    unbind.binding.boot_id = "boot-A";
    unbind.binding.version = 3;
    invalid = unbind;
    invalid.binding.user_guid = "user-B";
    assert(store.Apply(invalid, error) == BindingCommandResult::Rejected);
    assert(store.Apply(unbind, error) == BindingCommandResult::Accepted);
    assert(store.Get().state == BindingState::Unbound);
    assert(store.Get().version == 3);
    assert(!store.Get().IsBound());
    assert(store.Get().token.empty() && store.Get().usage_session_id.empty());
    assert(store.Apply(unbind, error) == BindingCommandResult::Duplicate);
    assert(store.Apply(second, error) == BindingCommandResult::Rejected);
    assert(!store.SetState(second.binding, BindingState::Ready));
    assert(store.Apply(Bind(4), error) == BindingCommandResult::Accepted);

    BootUserBinding rebooted("boot-B");
    assert(!rebooted.Get().IsBound() && rebooted.Get().version == 0);
    assert(rebooted.Apply(Bind(4), error) == BindingCommandResult::Rejected);
    auto new_boot = Bind();
    new_boot.binding.boot_id = "boot-B";
    assert(rebooted.Apply(new_boot, error) == BindingCommandResult::Accepted);
    std::cout << "boot binding state tests passed\n";
}
