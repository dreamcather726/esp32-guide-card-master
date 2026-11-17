package xiaozhi.modules.integration.service;

import java.util.List;
import java.util.Objects;

import org.apache.commons.lang3.StringUtils;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.exception.RenException;
import xiaozhi.modules.agent.dto.AgentRoleVoiceBindingDTO;
import xiaozhi.modules.agent.entity.AgentEntity;
import xiaozhi.modules.agent.service.AgentRoleVoiceBindingService;
import xiaozhi.modules.agent.service.AgentService;
import xiaozhi.modules.integration.dao.DeviceIntegrationDao;
import xiaozhi.modules.integration.dto.DeviceIntegrationDTOs;
import xiaozhi.modules.integration.dto.DeviceIntegrationDTOs.*;
import xiaozhi.modules.timbre.service.TimbreService;
import xiaozhi.modules.voiceclone.service.VoiceCloneService;

@Service
@RequiredArgsConstructor
public class DeviceRoleService {
    private final AgentService agents;
    private final AgentRoleVoiceBindingService roles;
    private final TimbreService timbres;
    private final VoiceCloneService clones;
    private final DeviceConversationService devices;
    private final DeviceIntegrationDao dao;

    private AgentEntity ownedAgent(Long owner, String id) {
        if (id == null || !id.matches("[A-Za-z0-9_-]{1,32}")) throw new RenException("Invalid agent ID");
        var agent = agents.selectById(id);
        if (agent == null || owner == null || !Objects.equals(owner, agent.getUserId())) {
            throw new RenException("Agent not available");
        }
        return agent;
    }

    public List<Role> roles(Long owner, String agentId) {
        var agent = ownedAgent(owner, agentId);
        return roles.listByAgentId(agentId).stream().map(role -> view(agent, role)).toList();
    }

    @Transactional(rollbackFor=Exception.class)
    public Role media(Long owner, String agentId, String roleId, Media media) {
        var agent = ownedAgent(owner, agentId);
        var role = roles.selectById(roleId);
        if (role == null || !Objects.equals(agentId, role.getAgentId())) throw new RenException("Role not available");
        role.setImageUrl(DeviceIntegrationDTOs.mediaUrl(media.imageUrl()));
        role.setVideoUrl(DeviceIntegrationDTOs.mediaUrl(media.videoUrl()));
        role.setAudioUrl(DeviceIntegrationDTOs.mediaUrl(media.audioUrl()));
        // Explicit nulls must clear URLs even when the ORM skips null-valued fields.
        roles.update(null, new com.baomidou.mybatisplus.core.conditions.update.UpdateWrapper<xiaozhi.modules.agent.entity.AgentRoleVoiceBindingEntity>()
                .eq("id", roleId).eq("agent_id", agentId)
                .set("image_url", role.getImageUrl()).set("video_url", role.getVideoUrl()).set("audio_url", role.getAudioUrl())
                .set("updated_at", new java.util.Date()).set("updater", owner));
        return roles(owner, agentId).stream().filter(r -> r.roleId().equals(roleId)).findFirst().orElseThrow();
    }

    @Transactional(rollbackFor=Exception.class)
    public RoleBinding bind(Long owner, String identifier, SelectRole request) {
        var device = devices.lockOwnedDevice(owner, identifier);
        ownedAgent(owner, request.agentId());
        if (!Objects.equals(device.getAgentId(), request.agentId())) {
            throw new RenException("Device must belong to the requested agent; this operation does not reassign devices");
        }
        var role = roles.selectById(request.roleId());
        if (role == null || !Objects.equals(request.agentId(), role.getAgentId()) || !Boolean.TRUE.equals(role.getEnabled())
                || (StringUtils.isBlank(role.getSystemPrompt()) && StringUtils.isBlank(role.getTtsVoiceId()))) {
            throw new RenException("Role is disabled, empty or not in the requested agent");
        }
        var current = dao.binding(device.getId());
        if (current == null || !request.roleId().equals(current.roleId())) dao.saveBinding(device, request.roleId());
        return dao.binding(device.getId());
    }

    public RoleBinding binding(Long owner, String identifier) {
        return dao.binding(devices.ownedDevice(owner, identifier).getId());
    }

    private Role view(AgentEntity agent, AgentRoleVoiceBindingDTO role) {
        String voice = StringUtils.isNotBlank(role.getTtsVoiceId()) ? role.getTtsVoiceId() : agent.getTtsVoiceId();
        String name = null, code = null;
        if (StringUtils.isNotBlank(voice)) {
            var timbre = timbres.get(voice);
            if (timbre != null) {
                name = timbre.getName(); code = timbre.getTtsVoice();
            } else {
                var clone = clones.selectById(voice);
                if (clone != null) { name = clone.getName(); code = clone.getVoiceId(); }
            }
        }
        return new Role(agent.getId(), role.getId(), role.getName(), role.getSystemPrompt(), voice,
                name, code, role.getImageUrl(), role.getVideoUrl(), role.getAudioUrl(), role.getSort(),
                Boolean.TRUE.equals(role.getEnabled()));
    }
}
