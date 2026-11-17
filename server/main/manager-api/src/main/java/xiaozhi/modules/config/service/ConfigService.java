package xiaozhi.modules.config.service;

import java.util.List;
import java.util.Map;

import xiaozhi.modules.config.dto.AgentRoleBindingResponseDTO;

public interface ConfigService {
    /**
     * 获取服务器配置
     *
     * @param isCache 是否缓存
     * @return 配置信息
     */
    Object getConfig(Boolean isCache);

    /**
     * 获取智能体模型配置
     *
     * @param macAddress     MAC地址
     * @param selectedModule 客户端已实例化的模型
     * @return 模型配置信息
     */
    Map<String, Object> getAgentModels(String macAddress, String userGuid,
            Map<String, String> selectedModule);

    String getAgentUserMemory(String agentId, String userGuid);

    /**
     * Get enabled role and voice bindings for an agent.
     *
     * @param agentId agent ID
     * @return enabled bindings ordered by configured sort order
     */
    List<AgentRoleBindingResponseDTO> getAgentRoleBindings(String agentId);

    /**
     * 获取智能体替换词
     *
     * @param macAddress 设备MAC地址
     * @return 替换词列表，格式如 ["模板1|模板01", "模板2|模板02"]
     */
    List<String> getCorrectWords(String macAddress);
}
