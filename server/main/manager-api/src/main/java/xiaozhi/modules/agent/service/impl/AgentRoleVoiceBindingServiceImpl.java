package xiaozhi.modules.agent.service.impl;

import java.util.Date;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

import org.apache.commons.lang3.StringUtils;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

import com.baomidou.mybatisplus.core.conditions.query.QueryWrapper;

import xiaozhi.common.service.impl.BaseServiceImpl;
import xiaozhi.modules.agent.dao.AgentRoleVoiceBindingDao;
import xiaozhi.modules.agent.dto.AgentRoleVoiceBindingDTO;
import xiaozhi.modules.agent.entity.AgentRoleVoiceBindingEntity;
import xiaozhi.modules.agent.service.AgentRoleVoiceBindingService;

@Service
public class AgentRoleVoiceBindingServiceImpl
        extends BaseServiceImpl<AgentRoleVoiceBindingDao, AgentRoleVoiceBindingEntity>
        implements AgentRoleVoiceBindingService {

    @Override
    public List<AgentRoleVoiceBindingDTO> listByAgentId(String agentId) {
        List<AgentRoleVoiceBindingEntity> entities = baseDao.selectList(
                new QueryWrapper<AgentRoleVoiceBindingEntity>()
                        .eq("agent_id", agentId)
                        .orderByAsc("sort")
                        .orderByAsc("created_at"));
        return entities.stream().map(this::toDTO).collect(Collectors.toList());
    }

    @Override
    @Transactional(rollbackFor = Exception.class)
    public void saveAgentBindings(String agentId, List<AgentRoleVoiceBindingDTO> bindings, Long userId) {
        Map<String, AgentRoleVoiceBindingEntity> previous = baseDao.selectList(
                new QueryWrapper<AgentRoleVoiceBindingEntity>().eq("agent_id", agentId)).stream()
                .collect(Collectors.toMap(AgentRoleVoiceBindingEntity::getId, item -> item));
        if (bindings != null) {
            for (var binding : bindings) {
                if (binding == null) continue;
                var old = previous.get(binding.getId());
                // Older console versions omit media fields when saving the whole role list.
                if (old != null) {
                    if (binding.getImageUrl() == null) binding.setImageUrl(old.getImageUrl());
                    if (binding.getVideoUrl() == null) binding.setVideoUrl(old.getVideoUrl());
                    if (binding.getAudioUrl() == null) binding.setAudioUrl(old.getAudioUrl());
                }
            }
        }
        deleteByAgentId(agentId);
        if (bindings == null || bindings.isEmpty()) {
            return;
        }

        Date now = new Date();
        List<AgentRoleVoiceBindingEntity> entities = bindings.stream()
                .filter(this::hasBindingContent)
                .map(binding -> toEntity(agentId, binding, userId, now))
                .collect(Collectors.toList());
        if (!entities.isEmpty()) {
            insertBatch(entities);
        }
    }

    @Override
    public void deleteByAgentId(String agentId) {
        baseDao.delete(new QueryWrapper<AgentRoleVoiceBindingEntity>().eq("agent_id", agentId));
    }

    private boolean hasBindingContent(AgentRoleVoiceBindingDTO binding) {
        return binding != null
                && (StringUtils.isNotBlank(binding.getName())
                || StringUtils.isNotBlank(binding.getSystemPrompt())
                || StringUtils.isNotBlank(binding.getTtsVoiceId()));
    }

    private AgentRoleVoiceBindingDTO toDTO(AgentRoleVoiceBindingEntity entity) {
        AgentRoleVoiceBindingDTO dto = new AgentRoleVoiceBindingDTO();
        dto.setId(entity.getId());
        dto.setAgentId(entity.getAgentId());
        dto.setName(entity.getName());
        dto.setSystemPrompt(entity.getSystemPrompt());
        dto.setTtsVoiceId(entity.getTtsVoiceId());
        dto.setImageUrl(entity.getImageUrl());
        dto.setVideoUrl(entity.getVideoUrl());
        dto.setAudioUrl(entity.getAudioUrl());
        dto.setSort(entity.getSort());
        dto.setEnabled(Boolean.TRUE.equals(entity.getEnabled()));
        return dto;
    }

    private AgentRoleVoiceBindingEntity toEntity(String agentId, AgentRoleVoiceBindingDTO dto, Long userId, Date now) {
        AgentRoleVoiceBindingEntity entity = new AgentRoleVoiceBindingEntity();
        if (StringUtils.isNotBlank(dto.getId())) {
            entity.setId(dto.getId());
        }
        entity.setAgentId(agentId);
        entity.setName(StringUtils.trimToNull(dto.getName()));
        entity.setSystemPrompt(StringUtils.trimToNull(dto.getSystemPrompt()));
        entity.setTtsVoiceId(StringUtils.trimToNull(dto.getTtsVoiceId()));
        entity.setImageUrl(xiaozhi.modules.integration.dto.DeviceIntegrationDTOs.mediaUrl(dto.getImageUrl()));
        entity.setVideoUrl(xiaozhi.modules.integration.dto.DeviceIntegrationDTOs.mediaUrl(dto.getVideoUrl()));
        entity.setAudioUrl(xiaozhi.modules.integration.dto.DeviceIntegrationDTOs.mediaUrl(dto.getAudioUrl()));
        entity.setSort(dto.getSort() == null ? 0 : dto.getSort());
        entity.setEnabled(dto.getEnabled() == null || dto.getEnabled());
        entity.setCreator(userId);
        entity.setCreatedAt(now);
        entity.setUpdater(userId);
        entity.setUpdatedAt(now);
        return entity;
    }
}
