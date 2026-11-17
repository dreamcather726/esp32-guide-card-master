package xiaozhi.modules.agent.service.impl;

import java.util.Date;

import org.apache.commons.lang3.StringUtils;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

import com.baomidou.mybatisplus.core.conditions.query.LambdaQueryWrapper;
import com.baomidou.mybatisplus.extension.service.impl.ServiceImpl;

import xiaozhi.modules.agent.dao.AgentUserMemoryDao;
import xiaozhi.modules.agent.entity.AgentUserMemoryEntity;
import xiaozhi.modules.agent.service.AgentUserMemoryService;

@Service
public class AgentUserMemoryServiceImpl
        extends ServiceImpl<AgentUserMemoryDao, AgentUserMemoryEntity>
        implements AgentUserMemoryService {

    @Override
    public String getSummaryMemory(String agentId, String userGuid) {
        String normalized = StringUtils.trimToNull(userGuid);
        if (StringUtils.isBlank(agentId) || normalized == null) {
            return null;
        }
        AgentUserMemoryEntity entity = getOne(new LambdaQueryWrapper<AgentUserMemoryEntity>()
                .eq(AgentUserMemoryEntity::getAgentId, agentId)
                .eq(AgentUserMemoryEntity::getUserGuid, normalized)
                .last("LIMIT 1"), false);
        return entity == null ? null : entity.getSummaryMemory();
    }

    @Override
    @Transactional(rollbackFor = Exception.class)
    public void saveOrUpdateSummaryMemory(String agentId, String userGuid, String summaryMemory) {
        String normalized = StringUtils.trimToNull(userGuid);
        if (StringUtils.isBlank(agentId) || normalized == null) {
            return;
        }

        AgentUserMemoryEntity entity = getOne(new LambdaQueryWrapper<AgentUserMemoryEntity>()
                .eq(AgentUserMemoryEntity::getAgentId, agentId)
                .eq(AgentUserMemoryEntity::getUserGuid, normalized)
                .last("LIMIT 1"), false);
        Date now = new Date();
        if (entity == null) {
            entity = new AgentUserMemoryEntity();
            entity.setAgentId(agentId);
            entity.setUserGuid(normalized);
            entity.setCreatedAt(now);
        }
        entity.setSummaryMemory(summaryMemory);
        entity.setUpdatedAt(now);
        saveOrUpdate(entity);
    }
}
