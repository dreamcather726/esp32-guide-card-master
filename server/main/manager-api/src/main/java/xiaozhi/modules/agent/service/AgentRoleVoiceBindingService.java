package xiaozhi.modules.agent.service;

import java.util.List;

import xiaozhi.common.service.BaseService;
import xiaozhi.modules.agent.dto.AgentRoleVoiceBindingDTO;
import xiaozhi.modules.agent.entity.AgentRoleVoiceBindingEntity;

public interface AgentRoleVoiceBindingService extends BaseService<AgentRoleVoiceBindingEntity> {
    List<AgentRoleVoiceBindingDTO> listByAgentId(String agentId);

    void saveAgentBindings(String agentId, List<AgentRoleVoiceBindingDTO> bindings, Long userId);

    void deleteByAgentId(String agentId);
}
