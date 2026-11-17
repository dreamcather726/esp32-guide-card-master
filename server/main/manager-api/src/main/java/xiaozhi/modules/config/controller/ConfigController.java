package xiaozhi.modules.config.controller;

import java.util.List;

import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import io.swagger.v3.oas.annotations.Operation;
import io.swagger.v3.oas.annotations.tags.Tag;
import jakarta.validation.Valid;
import lombok.AllArgsConstructor;
import xiaozhi.common.utils.Result;
import xiaozhi.common.validator.ValidatorUtils;
import xiaozhi.modules.config.dto.AgentModelsDTO;
import xiaozhi.modules.config.dto.AgentRoleBindingResponseDTO;
import xiaozhi.modules.config.dto.AgentUserMemoryDTO;
import xiaozhi.modules.config.dto.CorrectWordsDTO;
import xiaozhi.modules.config.service.ConfigService;
import xiaozhi.modules.binding.dto.UserBindingDTOs.Binding;
import xiaozhi.modules.binding.dto.UserBindingDTOs.Resolve;
import xiaozhi.modules.binding.service.UserBindingService;

/**
 * xiaozhi-server 配置获取
 *
 * @since 1.0.0
 */
@RestController
@RequestMapping("config")
@Tag(name = "参数管理")
@AllArgsConstructor
public class ConfigController {
    private final ConfigService configService;
    private final UserBindingService userBindingService;

    @PostMapping("user-binding/resolve")
    public Result<Binding> resolveUserBinding(@Valid @RequestBody Resolve request) {
        return new Result<Binding>().ok(userBindingService.resolve(request));
    }

    @PostMapping("server-base")
    @Operation(summary = "服务端获取配置接口")
    public Result<Object> getConfig() {
        Object config = configService.getConfig(true);
        return new Result<Object>().ok(config);
    }

    @PostMapping("agent-models")
    @Operation(summary = "获取智能体模型")
    public Result<Object> getAgentModels(@Valid @RequestBody AgentModelsDTO dto) {
        // 效验数据
        ValidatorUtils.validateEntity(dto);
        Object models = configService.getAgentModels(
                dto.getMacAddress(), dto.getUserGuid(), dto.getSelectedModule());
        return new Result<Object>().ok(models);
    }

    @PostMapping("agent-user-memory")
    @Operation(summary = "Get memory summary by agentId and userGUID")
    public Result<String> getAgentUserMemory(@Valid @RequestBody AgentUserMemoryDTO dto) {
        ValidatorUtils.validateEntity(dto);
        return new Result<String>().ok(
                configService.getAgentUserMemory(dto.getAgentId(), dto.getUserGuid()));
    }

    @GetMapping("agent-role-bindings/{agentId}")
    @Operation(summary = "Get enabled role and voice bindings by agent ID")
    public Result<List<AgentRoleBindingResponseDTO>> getAgentRoleBindings(
            @PathVariable("agentId") String agentId) {
        List<AgentRoleBindingResponseDTO> bindings = configService.getAgentRoleBindings(agentId);
        return new Result<List<AgentRoleBindingResponseDTO>>().ok(bindings);
    }

    @PostMapping("correct-words")
    @Operation(summary = "获取智能体替换词")
    public Result<Object> getCorrectWords(@Valid @RequestBody CorrectWordsDTO dto) {
        ValidatorUtils.validateEntity(dto);
        List<String> list = configService.getCorrectWords(dto.getMacAddress());
        return new Result<Object>().ok(list);
    }
}
