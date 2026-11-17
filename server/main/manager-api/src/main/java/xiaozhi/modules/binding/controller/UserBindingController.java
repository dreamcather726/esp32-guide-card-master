package xiaozhi.modules.binding.controller;

import java.util.List;

import org.apache.shiro.authz.annotation.RequiresPermissions;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import jakarta.validation.Valid;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.utils.Result;
import xiaozhi.modules.agent.entity.AgentChatHistoryEntity;
import xiaozhi.modules.binding.dto.UserBindingDTOs.*;
import xiaozhi.modules.binding.entity.UsageSessionEntity;
import xiaozhi.modules.binding.service.UserBindingService;
import xiaozhi.modules.security.user.SecurityUser;

@RestController
@RequiredArgsConstructor
@RequiresPermissions("sys:role:normal")
public class UserBindingController {
    private final UserBindingService service;

    @PostMapping("/device/{macAddress}/user-binding")
    public Result<Binding> create(@PathVariable String macAddress, @Valid @RequestBody Create request) {
        return new Result<Binding>().ok(service.create(SecurityUser.getUserId(), macAddress, request));
    }

    @GetMapping("/device/{macAddress}/user-binding")
    public Result<Binding> current(@PathVariable String macAddress) {
        return new Result<Binding>().ok(service.current(SecurityUser.getUserId(), macAddress));
    }

    @DeleteMapping("/device/{macAddress}/user-binding")
    public Result<Command> revoke(@PathVariable String macAddress) {
        return new Result<Command>().ok(service.revoke(SecurityUser.getUserId(), macAddress));
    }

    @GetMapping("/user/profile/{userGUID}/sessions")
    public Result<List<UsageSessionEntity>> sessions(@PathVariable("userGUID") String guid,
            @RequestParam(defaultValue = "1") int page, @RequestParam(defaultValue = "50") int limit) {
        return new Result<List<UsageSessionEntity>>().ok(service.sessions(SecurityUser.getUserId(), guid, page, limit));
    }

    @GetMapping("/user/profile/{userGUID}/sessions/{usageSessionID}/messages")
    public Result<List<AgentChatHistoryEntity>> messages(@PathVariable("userGUID") String guid,
            @PathVariable("usageSessionID") String session,
            @RequestParam(defaultValue = "1") int page, @RequestParam(defaultValue = "50") int limit) {
        return new Result<List<AgentChatHistoryEntity>>().ok(
                service.messages(SecurityUser.getUserId(), guid, session, page, limit));
    }
}
