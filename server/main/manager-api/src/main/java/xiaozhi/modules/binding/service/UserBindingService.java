package xiaozhi.modules.binding.service;

import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Objects;
import java.util.UUID;

import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import lombok.RequiredArgsConstructor;
import xiaozhi.common.exception.RenException;
import xiaozhi.common.validator.ValidatorUtils;
import xiaozhi.modules.agent.entity.AgentChatHistoryEntity;
import xiaozhi.modules.agent.entity.AgentEntity;
import xiaozhi.modules.agent.service.AgentService;
import xiaozhi.modules.binding.dao.UserBindingDao;
import xiaozhi.modules.binding.dto.UserBindingDTOs.*;
import xiaozhi.modules.binding.entity.DeviceBindingState;
import xiaozhi.modules.binding.entity.UsageSessionEntity;
import xiaozhi.modules.binding.entity.UserProfileEntity;
import xiaozhi.modules.device.entity.DeviceEntity;

@Service
@RequiredArgsConstructor
public class UserBindingService {
    private final UserBindingDao dao;
    private final AgentService agentService;
    private final ObjectMapper objectMapper;

    @Value("${user-binding.token-ttl-seconds:86400}")
    private long tokenTtlSeconds = 86400;

    public ProfileView getProfile(Long owner, String guid) {
        requireOwner(owner);
        requireIdentifier(guid, 128);
        UserProfileEntity profile = dao.profile(owner, guid);
        return profile == null ? null : profileView(profile);
    }

    public List<ProfileView> searchProfiles(Long owner, String prefix) {
        requireOwner(owner);
        requireIdentifier(prefix, 128);
        // Treat LIKE metacharacters as literal ID characters, not client-supplied wildcards.
        String pattern = prefix.replace("!", "!!").replace("%", "!%").replace("_", "!_") + "%";
        return dao.profilesByPrefix(owner, prefix, pattern).stream().map(this::profileView).toList();
    }

    @Transactional(rollbackFor = Exception.class)
    public ProfileView upsertProfile(Long owner, String guid, Profile profile) {
        requireOwner(owner);
        requireIdentifier(guid, 128);
        ValidatorUtils.validateEntity(profile);
        UserProfileEntity entity = new UserProfileEntity();
        entity.setOwnerId(owner);
        entity.setUserGuid(guid);
        entity.setNickname(profile.nickname());
        entity.setLanguage(profile.language());
        entity.setInterests(profile.interests());
        entity.setExplanationStyle(profile.explanationStyle());
        entity.setRelationship(profile.relationship());
        entity.setUsageCount(profile.usageCount());
        dao.upsertProfile(entity);
        return profileView(dao.profile(owner, guid));
    }

    @Transactional(rollbackFor = Exception.class)
    public Binding create(Long owner, String mac, Create request) {
        requireOwner(owner);
        ValidatorUtils.validateEntity(request);
        requireIdentifier(request.userGuid(), 128);
        requireIdentifier(request.bootId(), 128);
        mac = normalizeMac(mac);
        DeviceEntity device = ownedDevice(owner, mac);
        AgentEntity agent = agentService.selectById(device.getAgentId());
        if (agent == null || !Objects.equals(owner, agent.getUserId())) {
            throw new RenException("Device agent not available");
        }
        DeviceBindingState state = state(mac);
        ProfileView profile;
        if (request.profile() != null) {
            profile = upsertProfile(owner, request.userGuid(), request.profile());
        } else {
            dao.ensureProfile(owner, request.userGuid());
            profile = profileView(dao.profile(owner, request.userGuid()));
        }
        revokeActive(state);
        long version = Math.incrementExact(state.getBindingVersion());
        String token = BindingTokens.generate();
        Date now = new Date();
        UsageSessionEntity session = new UsageSessionEntity();
        session.setId(UUID.randomUUID().toString());
        session.setMacAddress(mac);
        session.setDeviceId(device.getId());
        session.setOwnerId(owner);
        session.setUserGuid(request.userGuid());
        session.setBootId(request.bootId());
        session.setBindingVersion(version);
        session.setAgentId(agent.getId());
        session.setAgentName(agent.getAgentName());
        session.setProfileSnapshot(profileJson(profile.profile()));
        session.setProfileVersion(profile.profileVersion());
        session.setTokenHash(BindingTokens.hash(token));
        session.setCreatedAt(now);
        if (tokenTtlSeconds <= 0 || tokenTtlSeconds > 604800) {
            throw new IllegalStateException("user-binding.token-ttl-seconds must be between 1 and 604800");
        }
        session.setExpiresAt(new Date(now.getTime() + tokenTtlSeconds * 1000));
        dao.insertSession(session);
        state.setBindingVersion(version);
        state.setActiveSessionId(session.getId());
        dao.updateState(state);
        Command command = new Command("bindUser", UUID.randomUUID().toString(), session.getUserGuid(),
                session.getBootId(), version, session.getId(), token);
        return binding(session, token, command);
    }

    @Transactional(rollbackFor = Exception.class)
    public Binding current(Long owner, String mac) {
        requireOwner(owner);
        mac = normalizeMac(mac);
        DeviceEntity device = ownedDevice(owner, mac);
        DeviceBindingState state = state(mac);
        UsageSessionEntity session = state.getActiveSessionId() == null ? null : dao.session(state.getActiveSessionId());
        if (!active(session, device, state)) {
            return null;
        }
        return binding(session, null, null);
    }

    @Transactional(rollbackFor = Exception.class)
    public Command revoke(Long owner, String mac) {
        requireOwner(owner);
        mac = normalizeMac(mac);
        ownedDevice(owner, mac);
        DeviceBindingState state = state(mac);
        UsageSessionEntity previous = state.getActiveSessionId() == null ? null : dao.session(state.getActiveSessionId());
        revokeActive(state);
        state.setBindingVersion(Math.incrementExact(state.getBindingVersion()));
        state.setActiveSessionId(null);
        dao.updateState(state);
        return previous == null ? null : new Command("unbindUser", UUID.randomUUID().toString(), "",
                previous.getBootId(), state.getBindingVersion(), null, null);
    }

    @Transactional(rollbackFor = Exception.class)
    public Binding resolve(Resolve request) {
        ValidatorUtils.validateEntity(request);
        String mac = normalizeMac(request.macAddress());
        DeviceEntity device = dao.lockDevice(mac);
        DeviceBindingState state = dao.lockState(mac);
        UsageSessionEntity session = state == null || state.getActiveSessionId() == null
                ? null : dao.session(state.getActiveSessionId());
        if (!active(session, device, state)
                || !Objects.equals(session.getUserGuid(), request.userGuid())
                || !Objects.equals(session.getBootId(), request.bootId())
                || !Objects.equals(session.getBindingVersion(), request.bindingVersion())
                || !BindingTokens.matches(request.bindingToken(), session.getTokenHash())) {
            throw new RenException("Invalid or expired user binding");
        }
        AgentEntity agent = agentService.selectById(session.getAgentId());
        if (agent == null || !Objects.equals(session.getOwnerId(), agent.getUserId())) {
            throw new RenException("Invalid or expired user binding");
        }
        return binding(session, null, null);
    }

    public List<UsageSessionEntity> sessions(Long owner, String guid, int page, int limit) {
        requireOwner(owner);
        requireIdentifier(guid, 128);
        return dao.sessions(owner, guid, checkedLimit(limit), offset(page, limit));
    }

    public List<AgentChatHistoryEntity> messages(Long owner, String guid, String sessionId, int page, int limit) {
        requireOwner(owner);
        requireIdentifier(guid, 128);
        UsageSessionEntity session = dao.session(sessionId);
        if (session == null || !Objects.equals(owner, session.getOwnerId())
                || !Objects.equals(guid, session.getUserGuid())) {
            throw new RenException("Usage session not available");
        }
        return dao.messages(owner, guid, sessionId, checkedLimit(limit), offset(page, limit));
    }

    // Delayed trusted reports may arrive after revoke or expiry. Keep their original identity snapshots.
    @Transactional(rollbackFor = Exception.class)
    public UsageSessionEntity validateReport(String mac, String guid, String bootId, String sessionId, Long bindingVersion) {
        mac = normalizeMac(mac);
        requireIdentifier(guid, 128);
        requireIdentifier(bootId, 128);
        requireIdentifier(sessionId, 36);
        UsageSessionEntity session = dao.lockSession(sessionId);
        if (session == null || !Objects.equals(mac, session.getMacAddress())
                || !Objects.equals(guid, session.getUserGuid()) || !Objects.equals(bootId, session.getBootId())
                || bindingVersion == null || !Objects.equals(bindingVersion, session.getBindingVersion())) {
            throw new RenException("Usage session does not match report identity");
        }
        return session;
    }

    public static String normalizeMac(String mac) {
        if (mac == null || !mac.matches("(?i)[0-9a-f]{2}([:-][0-9a-f]{2}){5}")) {
            throw new RenException("Invalid MAC address");
        }
        return mac.replace('-', ':').toLowerCase(Locale.ROOT);
    }

    private DeviceEntity ownedDevice(Long owner, String mac) {
        DeviceEntity device = dao.lockDevice(mac);
        if (device == null || !Objects.equals(owner, device.getUserId())) {
            throw new RenException("Device not available");
        }
        return device;
    }

    private DeviceBindingState state(String mac) {
        dao.ensureState(mac);
        return dao.lockState(mac);
    }

    private void revokeActive(DeviceBindingState state) {
        if (state.getActiveSessionId() != null) {
            dao.revokeSession(state.getActiveSessionId());
        }
    }

    private boolean active(UsageSessionEntity session, DeviceEntity device, DeviceBindingState state) {
        return session != null && device != null && state != null && session.getRevokedAt() == null
                && session.getExpiresAt().after(new Date())
                && Objects.equals(session.getBindingVersion(), state.getBindingVersion())
                && Objects.equals(session.getDeviceId(), device.getId())
                && Objects.equals(session.getOwnerId(), device.getUserId())
                && Objects.equals(session.getAgentId(), device.getAgentId());
    }

    private Binding binding(UsageSessionEntity session, String token, Command command) {
        try {
            Profile profile = objectMapper.readValue(session.getProfileSnapshot(), Profile.class);
            return new Binding(session.getUserGuid(), session.getBootId(), session.getBindingVersion(), session.getId(),
                    token, profile, session.getProfileVersion(), session.getAgentId(), session.getExpiresAt(), command);
        } catch (JsonProcessingException e) {
            throw new IllegalStateException("Stored profile snapshot is invalid", e);
        }
    }

    private ProfileView profileView(UserProfileEntity profile) {
        return new ProfileView(profile.getUserGuid(), profile.getProfileVersion(), new Profile(profile.getNickname(),
                profile.getLanguage(), profile.getInterests(), profile.getExplanationStyle(),
                profile.getRelationship(), profile.getUsageCount()));
    }

    private String profileJson(Profile profile) {
        try {
            return objectMapper.writeValueAsString(profile);
        } catch (JsonProcessingException e) {
            throw new IllegalStateException("Profile serialization failed", e);
        }
    }

    private void requireOwner(Long owner) {
        if (owner == null) {
            throw new RenException("Login required");
        }
    }

    private void requireIdentifier(String value, int max) {
        if (value == null || value.isBlank() || value.length() > max || !value.equals(value.trim())) {
            throw new RenException("Invalid identifier");
        }
    }

    private int checkedLimit(int limit) {
        if (limit < 1 || limit > 100) {
            throw new RenException("limit must be between 1 and 100");
        }
        return limit;
    }

    private int offset(int page, int limit) {
        if (page < 1 || page > 1000000) {
            throw new RenException("Invalid page");
        }
        return (page - 1) * checkedLimit(limit);
    }
}
