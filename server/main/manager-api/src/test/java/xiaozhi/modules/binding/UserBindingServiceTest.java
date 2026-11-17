package xiaozhi.modules.binding;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.ArgumentMatchers.*;
import static org.mockito.Mockito.*;

import java.util.Date;
import java.util.HashMap;
import java.util.Map;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import xiaozhi.common.exception.RenException;
import xiaozhi.modules.agent.entity.AgentEntity;
import xiaozhi.modules.agent.service.AgentService;
import xiaozhi.modules.binding.dao.UserBindingDao;
import xiaozhi.modules.binding.dto.UserBindingDTOs.*;
import xiaozhi.modules.binding.entity.DeviceBindingState;
import xiaozhi.modules.binding.entity.UsageSessionEntity;
import xiaozhi.modules.binding.entity.UserProfileEntity;
import xiaozhi.modules.binding.service.BindingTokens;
import xiaozhi.modules.binding.service.UserBindingService;
import xiaozhi.modules.device.entity.DeviceEntity;
import xiaozhi.modules.security.config.ShiroConfig;
import xiaozhi.modules.sys.service.SysParamsService;

class UserBindingServiceTest {
    private static final String MAC = "aa:bb:cc:dd:ee:ff";
    private UserBindingDao dao;
    private UserBindingService service;
    private DeviceEntity device;
    private DeviceBindingState state;
    private ObjectMapper json;
    private final Map<String, UsageSessionEntity> sessions = new HashMap<>();

    @BeforeEach
    void setup() {
        sessions.clear();
        dao = mock(UserBindingDao.class);
        AgentService agents = mock(AgentService.class);
        json = new ObjectMapper();
        service = new UserBindingService(dao, agents, json);
        device = new DeviceEntity();
        device.setId("device-1");
        device.setMacAddress(MAC);
        device.setUserId(10L);
        device.setAgentId("agent-1");
        when(dao.lockDevice(MAC)).thenReturn(device);
        AgentEntity agent = new AgentEntity();
        agent.setId("agent-1");
        agent.setUserId(10L);
        agent.setAgentName("Guide");
        when(agents.selectById("agent-1")).thenReturn(agent);
        state = new DeviceBindingState();
        state.setMacAddress(MAC);
        state.setBindingVersion(0L);
        when(dao.lockState(MAC)).thenReturn(state);
        doAnswer(call -> {
            UsageSessionEntity session = call.getArgument(0);
            sessions.put(session.getId(), session);
            return null;
        }).when(dao).insertSession(any());
        when(dao.session(anyString())).thenAnswer(call -> sessions.get(call.getArgument(0)));
        when(dao.lockSession(anyString())).thenAnswer(call -> sessions.get(call.getArgument(0)));
        doAnswer(call -> {
            sessions.get(call.getArgument(0)).setRevokedAt(new Date());
            return null;
        }).when(dao).revokeSession(anyString());
        UserProfileEntity profile = new UserProfileEntity();
        profile.setOwnerId(10L);
        profile.setUserGuid("user-1");
        profile.setNickname("Alice");
        profile.setProfileVersion(1L);
        when(dao.profile(10L, "user-1")).thenReturn(profile);
    }

    private Binding create() {
        return service.create(10L, MAC, new Create("boot-1", "user-1", null));
    }

    private Resolve resolveRequest(Binding binding) {
        return new Resolve(MAC, binding.userGuid(), binding.bootId(), binding.bindingVersion(), binding.bindingToken());
    }

    @Test
    void issues256BitTokenAndPersistsOnlyItsHash() {
        Binding binding = create();
        assertEquals(43, binding.bindingToken().length());
        assertEquals(32, java.util.Base64.getUrlDecoder().decode(binding.bindingToken()).length);
        UsageSessionEntity session = sessions.get(binding.usageSessionId());
        assertNotEquals(binding.bindingToken(), session.getTokenHash());
        assertTrue(BindingTokens.matches(binding.bindingToken(), session.getTokenHash()));
        assertEquals("bindUser", binding.command().cmd());
        assertEquals(binding.bindingToken(), binding.command().bindingToken());
        assertEquals(1L, binding.bindingVersion());
    }

    @Test
    void repeatedResolveKeepsSessionAndNeverReturnsToken() {
        Binding issued = create();
        Binding first = service.resolve(resolveRequest(issued));
        Binding second = service.resolve(resolveRequest(issued));
        assertEquals(issued.usageSessionId(), first.usageSessionId());
        assertEquals(first.usageSessionId(), second.usageSessionId());
        assertNull(first.bindingToken());
        assertNull(first.command());
        assertEquals("Alice", first.profile().nickname());
        verify(dao, times(1)).insertSession(any());
    }

    @Test
    void newBindingInvalidatesPreviousAndIncrementsVersion() {
        Binding first = create();
        Binding second = create();
        assertEquals(first.bindingVersion() + 1, second.bindingVersion());
        assertNotEquals(first.usageSessionId(), second.usageSessionId());
        assertNotEquals(first.bindingToken(), second.bindingToken());
        assertThrows(RenException.class, () -> service.resolve(resolveRequest(first)));
        assertEquals(second.usageSessionId(), service.resolve(resolveRequest(second)).usageSessionId());
    }

    @Test
    void revokeReservesUnbindVersionAndSubsequentCreateAdvancesIt() throws Exception {
        Binding first = create();
        Command command = service.revoke(10L, MAC);
        assertEquals("unbindUser", command.cmd());
        assertEquals("", command.userGuid());
        assertEquals(first.bootId(), command.bootId());
        assertEquals(first.bindingVersion() + 1, command.bindingVersion());
        JsonNode body = json.valueToTree(command);
        assertFalse(body.has("bindingToken"));
        assertFalse(body.has("usageSessionID"));
        assertThrows(RenException.class, () -> service.resolve(resolveRequest(first)));
        assertNull(service.current(10L, MAC));
        assertEquals(command.bindingVersion() + 1, create().bindingVersion());
    }

    @Test
    void revokeWithoutBindingReturnsNoCommand() {
        assertNull(service.revoke(10L, MAC));
    }

    @Test
    void rejectsCrossOwnerAndMissingLoginBeforeIssuingToken() {
        assertThrows(RenException.class, () -> service.create(20L, MAC, new Create("b", "u", null)));
        assertThrows(RenException.class, () -> service.create(null, MAC, new Create("b", "u", null)));
        assertThrows(RenException.class, () -> service.current(20L, MAC));
        assertThrows(RenException.class, () -> service.revoke(20L, MAC));
        verify(dao, never()).insertSession(any());
    }

    @Test
    void rejectsBootUserVersionAndTokenMismatch() {
        Binding issued = create();
        assertThrows(RenException.class, () -> service.resolve(new Resolve(MAC, "other", "boot-1", 1L, issued.bindingToken())));
        assertThrows(RenException.class, () -> service.resolve(new Resolve(MAC, "user-1", "other", 1L, issued.bindingToken())));
        assertThrows(RenException.class, () -> service.resolve(new Resolve(MAC, "user-1", "boot-1", 2L, issued.bindingToken())));
        assertThrows(RenException.class, () -> service.resolve(new Resolve(MAC, "user-1", "boot-1", 1L, BindingTokens.generate())));
    }

    @Test
    void expiryAndOwnershipTransferInvalidateTokens() {
        Binding issued = create();
        sessions.get(issued.usageSessionId()).setExpiresAt(new Date(0));
        assertThrows(RenException.class, () -> service.resolve(resolveRequest(issued)));
        sessions.get(issued.usageSessionId()).setExpiresAt(new Date(System.currentTimeMillis() + 60000));
        device.setUserId(20L);
        assertThrows(RenException.class, () -> service.resolve(resolveRequest(issued)));
    }

    @Test
    void recreatedDeviceOrChangedAgentInvalidatesToken() {
        Binding issued = create();
        device.setId("new-device-id");
        assertThrows(RenException.class, () -> service.resolve(resolveRequest(issued)));
        device.setId("device-1");
        device.setAgentId("new-agent");
        assertThrows(RenException.class, () -> service.resolve(resolveRequest(issued)));
    }

    @Test
    void profileIsOwnerScopedAndLengthBounded() {
        assertNotNull(service.getProfile(10L, "user-1"));
        assertNull(service.getProfile(20L, "user-1"));
        assertThrows(RenException.class, () -> service.upsertProfile(10L, "user-1", new Profile("a".repeat(65), null, null, null)));
        assertThrows(RenException.class, () -> service.upsertProfile(10L, "user-1", new Profile(null, "a".repeat(33), null, null)));
        assertThrows(RenException.class, () -> service.upsertProfile(10L, "user-1", new Profile(null, null, "a".repeat(1025), null)));
        assertThrows(RenException.class, () -> service.upsertProfile(10L, "user-1", new Profile(null, null, null, "a".repeat(513))));
        verify(dao, never()).upsertProfile(any());
    }

    @Test
    void historyChecksOwnerUserAndSessionTogether() {
        Binding issued = create();
        assertThrows(RenException.class, () -> service.messages(20L, "user-1", issued.usageSessionId(), 1, 50));
        assertThrows(RenException.class, () -> service.messages(10L, "other-user", issued.usageSessionId(), 1, 50));
        service.messages(10L, "user-1", issued.usageSessionId(), 2, 50);
        verify(dao).messages(10L, "user-1", issued.usageSessionId(), 50, 50);
        assertThrows(RenException.class, () -> service.sessions(10L, "user-1", 1, 101));
    }

    @Test
    void delayedReportsRetainSessionSnapshotAfterRevokeButRejectWrongIdentity() {
        Binding issued = create();
        service.revoke(10L, MAC);
        UsageSessionEntity original = service.validateReport(MAC, "user-1", "boot-1", issued.usageSessionId(), issued.bindingVersion());
        assertEquals("agent-1", original.getAgentId());
        assertThrows(RenException.class, () -> service.validateReport("00:11:22:33:44:55", "user-1", "boot-1", issued.usageSessionId(), issued.bindingVersion()));
        assertThrows(RenException.class, () -> service.validateReport(MAC, "other", "boot-1", issued.usageSessionId(), issued.bindingVersion()));
        assertThrows(RenException.class, () -> service.validateReport(MAC, "user-1", "other", issued.usageSessionId(), issued.bindingVersion()));
        assertThrows(RenException.class, () -> service.validateReport(MAC, "user-1", "boot-1", issued.usageSessionId(), issued.bindingVersion() + 1));
        assertThrows(RenException.class, () -> service.validateReport(MAC, "user-1", "boot-1", issued.usageSessionId(), null));
    }

    @Test
    void profileEditsApplyOnlyToTheNextBinding() {
        Binding first = create();
        UserProfileEntity updated = new UserProfileEntity();
        updated.setOwnerId(10L);
        updated.setUserGuid("user-1");
        updated.setNickname("New name");
        updated.setProfileVersion(2L);
        when(dao.profile(10L, "user-1")).thenReturn(updated);
        assertEquals("New name", service.getProfile(10L, "user-1").profile().nickname());
        assertEquals("Alice", service.resolve(resolveRequest(first)).profile().nickname());
        assertEquals(1L, service.current(10L, MAC).profileVersion());
        Binding next = create();
        assertEquals("New name", next.profile().nickname());
        assertEquals(2L, next.profileVersion());
    }

    @Test
    void savesExternallySuppliedMetadataUnderTheAuthenticatedOwner() {
        service.upsertProfile(10L, "user-1", new Profile("Alice", "zh-CN", null, null, "self", 7));
        var saved = org.mockito.ArgumentCaptor.forClass(UserProfileEntity.class);
        verify(dao).upsertProfile(saved.capture());
        assertEquals(10L, saved.getValue().getOwnerId());
        assertEquals("user-1", saved.getValue().getUserGuid());
        assertEquals("self", saved.getValue().getRelationship());
        assertEquals(7, saved.getValue().getUsageCount());
    }

    @Test
    void metadataIsSnapshottedAndBindingDoesNotIncrementSuppliedCount() {
        var stored = dao.profile(10L, "user-1");
        stored.setRelationship("self");
        stored.setUsageCount(7);
        Binding first = create();
        assertEquals("self", first.profile().relationship());
        assertEquals(7, first.profile().usageCount());
        assertEquals(7, create().profile().usageCount());
        Binding active = create();
        stored.setRelationship("father");
        stored.setUsageCount(9);
        stored.setProfileVersion(2L);
        assertEquals(9, service.getProfile(10L, "user-1").profile().usageCount());
        assertEquals("self", service.resolve(resolveRequest(active)).profile().relationship());
        assertEquals(7, service.current(10L, MAC).profile().usageCount());
        Binding next = create();
        assertEquals("father", next.profile().relationship());
        assertEquals(9, next.profile().usageCount());
        verify(dao, never()).upsertProfile(any());
    }

    @Test
    void oldFourFieldSnapshotStillResolvesAfterUpgrade() {
        Binding issued = create();
        sessions.get(issued.usageSessionId()).setProfileSnapshot("{\"nickname\":\"Legacy\"}");
        var restored = service.resolve(resolveRequest(issued)).profile();
        assertEquals("Legacy", restored.nickname());
        assertNull(restored.relationship());
        assertNull(restored.usageCount());
    }

    @Test
    void historySqlOrdersByOriginalReportTimestampAndId() throws Exception {
        var select = UserBindingDao.class.getMethod("messages", Long.class, String.class, String.class, int.class, int.class)
                .getAnnotation(org.apache.ibatis.annotations.Select.class);
        assertTrue(String.join(" ", select.value()).contains("ORDER BY created_at ASC,id ASC"));
    }

    @Test
    void serializesExactWireNamesAndNoSecretsOnReads() throws Exception {
        Binding issued = create();
        JsonNode response = json.valueToTree(service.current(10L, MAC));
        assertTrue(response.has("userGUID"));
        assertTrue(response.has("bootID"));
        assertTrue(response.has("usageSessionID"));
        assertFalse(response.has("userGuid"));
        assertFalse(response.has("bindingToken"));
        assertFalse(response.has("command"));
        assertFalse(json.valueToTree(sessions.get(issued.usageSessionId())).has("tokenHash"));
        assertFalse(resolveRequest(issued).toString().contains(issued.bindingToken()));
        Resolve decoded = json.readValue(json.writeValueAsString(resolveRequest(issued)), Resolve.class);
        assertEquals(issued.bootId(), decoded.bootId());
    }

    @Test
    void inheritsExistingOperatorAndServerAuthenticationChains() {
        var factory = new ShiroConfig().shirFilter(mock(org.apache.shiro.mgt.SecurityManager.class), mock(SysParamsService.class),
                mock(xiaozhi.modules.binding.service.ProfileApiKeyService.class));
        var chains = factory.getFilterChainDefinitionMap();
        assertEquals("server", chains.get("/config/**"));
        assertEquals("server", chains.get("/agent/chat-history/report"));
        assertEquals("oauth2", chains.get("/**"));
        assertEquals("profile", chains.get("/user/profile/**"));
        var permissions = xiaozhi.modules.binding.controller.UserBindingController.class
                .getAnnotation(org.apache.shiro.authz.annotation.RequiresPermissions.class);
        assertArrayEquals(new String[]{"sys:role:normal"}, permissions.value());
    }
}
