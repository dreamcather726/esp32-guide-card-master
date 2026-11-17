package xiaozhi.modules.agent.service;

import com.baomidou.mybatisplus.extension.service.IService;

import xiaozhi.modules.agent.entity.AgentUserMemoryEntity;

public interface AgentUserMemoryService extends IService<AgentUserMemoryEntity> {
    String getSummaryMemory(String agentId, String userGuid);

    void saveOrUpdateSummaryMemory(String agentId, String userGuid, String summaryMemory);
}
