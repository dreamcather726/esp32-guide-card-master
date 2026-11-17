package xiaozhi.modules.integration.controller;

import java.util.List;
import java.util.Map;
import org.springframework.web.bind.annotation.*;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.validation.Valid;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.utils.Result;
import xiaozhi.modules.integration.dto.DeviceIntegrationDTOs.*;
import xiaozhi.modules.integration.service.*;
import xiaozhi.modules.security.oauth2.IntegrationApiKeyFilter;

@RestController
@RequestMapping("/integration")
@RequiredArgsConstructor
public class DeviceIntegrationController {
    private final DeviceRoleService roles;
    private final DeviceConversationService history;
    private final DeviceRuntimeService runtime;

    @GetMapping("/agents/{agentId}/roles")
    public Result<List<Role>> roles(@PathVariable String agentId, HttpServletRequest request) {
        return new Result<List<Role>>().ok(roles.roles(IntegrationApiKeyFilter.owner(request), agentId));
    }

    @PutMapping("/agents/{agentId}/roles/{roleId}/media")
    public Result<Role> media(@PathVariable String agentId, @PathVariable String roleId,
            @Valid @RequestBody Media media, HttpServletRequest request) {
        return new Result<Role>().ok(roles.media(IntegrationApiKeyFilter.owner(request), agentId, roleId, media));
    }

    @GetMapping("/devices/{deviceId}/role-binding")
    public Result<RoleBinding> binding(@PathVariable String deviceId, HttpServletRequest request) {
        return new Result<RoleBinding>().ok(roles.binding(IntegrationApiKeyFilter.owner(request), deviceId));
    }

    @PutMapping("/devices/{deviceId}/role-binding")
    public Result<BindingResult> bind(@PathVariable String deviceId, @Valid @RequestBody SelectRole selection,
            HttpServletRequest request) {
        var saved = roles.bind(IntegrationApiKeyFilter.owner(request), deviceId, selection);
        String status = runtime.apply(Map.of("action", "apply_role", "deviceId", saved.macAddress(),
                "agentId", saved.agentId(), "roleId", saved.roleId(), "bindingVersion", saved.bindingVersion()));
        return new Result<BindingResult>().ok(new BindingResult(saved, status));
    }

    @GetMapping("/devices/{deviceId}/chat-history")
    public Result<History> history(@PathVariable String deviceId,
            @RequestParam(defaultValue="1") int page, @RequestParam(defaultValue="30") int limit,
            HttpServletRequest request) {
        return new Result<History>().ok(history.history(IntegrationApiKeyFilter.owner(request), deviceId, page, limit));
    }

    @DeleteMapping("/devices/{deviceId}/chat-history")
    public Result<Cleared> clear(@PathVariable String deviceId,
            @RequestParam(defaultValue="false") boolean confirm, HttpServletRequest request) {
        Long owner = IntegrationApiKeyFilter.owner(request);
        if (!confirm) return new Result<Cleared>().error(400, "Clearing all device history requires confirm=true");
        var cleared = history.clear(owner, deviceId);
        String status = runtime.apply(Map.of("action", "clear_history", "deviceId", cleared.macAddress()));
        return new Result<Cleared>().ok(new Cleared(cleared.deviceId(), cleared.macAddress(),
                cleared.deletedCount(), cleared.clearedAt(), status));
    }
}
