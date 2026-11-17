package xiaozhi.modules.binding;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.ArgumentMatchers.*;
import static org.mockito.Mockito.*;

import java.time.Clock;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.Date;
import java.util.HashMap;
import java.util.Map;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.test.util.ReflectionTestUtils;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import xiaozhi.common.constant.Constant;
import xiaozhi.common.exception.RenException;
import xiaozhi.modules.agent.entity.AgentEntity;
import xiaozhi.modules.agent.service.AgentService;
import xiaozhi.modules.binding.dao.MqttBindingDao;
import xiaozhi.modules.binding.dao.UserBindingDao;
import xiaozhi.modules.binding.dto.MqttBindingDTOs.*;
import xiaozhi.modules.binding.dto.UserBindingDTOs.*;
import xiaozhi.modules.binding.entity.*;
import xiaozhi.modules.binding.service.*;
import xiaozhi.modules.device.entity.DeviceEntity;
import xiaozhi.modules.sys.service.SysParamsService;

class MqttBindingServiceTest {
    private static final String MAC = "aa:bb:cc:dd:ee:ff";
    private static final String CAB = "cab-1";
    private final ObjectMapper json = new ObjectMapper();
    private MqttBindingDao dao;
    private UserBindingDao devices;
    private UserBindingService bindings;
    private MqttBindingService service;
    private MqttBindingCipher cipher;
    private DeviceEntity device;
    private AgentEntity agent;
    private DeviceBindingState state;
    private MqttBindingMapping mapping;
    private final Map<String, MqttBindingOutbox> outbox = new HashMap<>();
    private final Map<String, UsageSessionEntity> sessions = new HashMap<>();
    private long now;

    @BeforeEach
    void setup() {
        now = System.currentTimeMillis();
        dao = mock(MqttBindingDao.class);
        devices = mock(UserBindingDao.class);
        AgentService agents = mock(AgentService.class);
        bindings = spy(new UserBindingService(devices, agents, json));
        SysParamsService params = mock(SysParamsService.class);
        when(params.getValue(Constant.SERVER_SECRET, true)).thenReturn("stable-test-server-secret");
        cipher = new MqttBindingCipher(params);
        service = new MqttBindingService(dao, devices, bindings, cipher, json);
        time(now);
        device = new DeviceEntity();
        device.setId("device-1");
        device.setMacAddress(MAC);
        device.setUserId(10L);
        device.setAgentId("agent-1");
        when(devices.lockDevice(MAC)).thenReturn(device);
        agent = new AgentEntity();
        agent.setId("agent-1");
        agent.setUserId(10L);
        when(agents.selectById("agent-1")).thenReturn(agent);
        state = new DeviceBindingState();
        state.setMacAddress(MAC);
        state.setBindingVersion(0L);
        when(devices.lockState(MAC)).thenReturn(state);
        doAnswer(call -> {
            UsageSessionEntity value = call.getArgument(0);
            sessions.put(value.getId(), value);
            return null;
        }).when(devices).insertSession(any());
        when(devices.session(anyString())).thenAnswer(call -> sessions.get(call.getArgument(0)));
        doAnswer(call -> {
            sessions.get(call.getArgument(0)).setRevokedAt(new Date());
            return null;
        }).when(devices).revokeSession(anyString());
        when(devices.profile(eq(10L), anyString())).thenAnswer(call -> {
            UserProfileEntity profile = new UserProfileEntity();
            profile.setUserGuid(call.getArgument(1));
            profile.setOwnerId(10L);
            profile.setNickname("Authoritative profile");
            profile.setProfileVersion(3L);
            return profile;
        });
        when(dao.lockMapping(MAC)).thenAnswer(call -> mapping);
        when(dao.byCab(anyString())).thenAnswer(call -> mapping != null
                && mapping.getCabGuid().equals(call.getArgument(0)) ? mapping : null);
        doAnswer(call -> mapping = call.getArgument(0)).when(dao).insertMapping(any());
        doAnswer(call -> mapping = call.getArgument(0)).when(dao).updateMapping(any());
        when(dao.request(eq(MAC), anyString(), anyString())).thenAnswer(call ->
                outbox.get(call.getArgument(1) + ":" + call.getArgument(2)));
        doAnswer(call -> {
            MqttBindingOutbox row = call.getArgument(0);
            outbox.put(row.getCabGuid() + ":" + row.getRequestId(), row);
            return null;
        }).when(dao).insertOutbox(any());
        when(dao.reusable(eq(MAC), anyString(), anyString(), anyString(), any())).thenAnswer(call ->
                outbox.values().stream().filter(row -> row.getCabGuid().equals(call.getArgument(1))
                        && row.getMappingId().equals(call.getArgument(2))
                        && row.getUsageSessionId().equals(call.getArgument(3))
                        && row.getEncryptedBinding() != null && row.getExpiresAt().after(call.getArgument(4)))
                        .findFirst().orElse(null));
        doAnswer(call -> {
            outbox.values().forEach(row -> row.setEncryptedBinding(null));
            return null;
        }).when(dao).invalidateCommands(MAC);
    }

    private void time(long millis) {
        ReflectionTestUtils.setField(service, "clock", Clock.fixed(Instant.ofEpochMilli(millis), ZoneOffset.UTC));
    }

    private ObjectNode report() {
        return json.createObjectNode().put("cmd", "KJ").put("cabGuid", CAB).put("bootID", "boot-1")
                .put("imei", "123456789012345").put("userGUID", "user-1").put("groupID", "group-1")
                .put("bindingState", "legacy_pending").put("bindingVersion", 0).put("usageSessionID", "")
                .put("fwVersion", "2.1.7").put("Battery Level", "75");
    }

    private Legacy request(String id) {
        return new Legacy(CAB, "boot-1", "user-1", id);
    }

    private void observed() {
        service.pair(10L, MAC, new Pair(CAB));
        service.online(report());
    }

    @Test
    void pairingRequiresRegisteredOwnerAndNormalizesMac() {
        assertThrows(RenException.class, () -> service.pair(null, MAC, new Pair(CAB)));
        assertThrows(RenException.class, () -> service.pair(20L, MAC, new Pair(CAB)));
        assertThrows(RenException.class, () -> service.pair(10L, "00:11:22:33:44:55", new Pair(CAB)));
        assertThrows(RenException.class, () -> service.current(20L, MAC));
        assertThrows(RenException.class, () -> service.pair(10L, MAC, new Pair(" cab ")));
        View paired = service.pair(10L, "AA-BB-CC-DD-EE-FF", new Pair(CAB));
        assertEquals(MAC, paired.macAddress());
        assertEquals(CAB, paired.cabGuid());
        assertNull(paired.report());
        assertNull(paired.receivedAt());
        assertFalse(paired.fresh());
        assertEquals(device.getId(), mapping.getDeviceId());
        assertEquals(10L, mapping.getOwnerId());
    }

    @Test
    void ownerReadWithoutPairingHasExactEmptyShape() {
        View empty = service.current(10L, MAC);
        assertEquals(new View(MAC, null, null, null, false), empty);
        assertEquals(5, json.valueToTree(empty).size());
        assertTrue(json.valueToTree(empty).get("report").isNull());
    }

    @Test
    void repeatedPairPreservesObservationButChangingPairClearsIt() {
        observed();
        String generation = mapping.getMappingId();
        assertTrue(service.pair(10L, MAC, new Pair(CAB)).fresh());
        assertEquals(generation, mapping.getMappingId());
        View changed = service.pair(10L, MAC, new Pair("cab-2"));
        assertFalse(changed.fresh());
        assertNull(changed.report());
        assertNotEquals(generation, mapping.getMappingId());
        assertThrows(RenException.class, () -> service.online(report()));
    }

    @Test
    void cabinetMayNotBeStolenFromAnotherMac() {
        MqttBindingMapping other = new MqttBindingMapping();
        other.setMacAddress("00:11:22:33:44:55");
        when(dao.byCab(CAB)).thenReturn(other);
        assertThrows(RenException.class, () -> service.pair(10L, MAC, new Pair(CAB)));
        verify(dao, never()).insertMapping(any());
    }

    @Test
    void onlineRequiresExplicitMappingAndChecksOptionalMac() {
        assertThrows(RenException.class, () -> service.online(report()));
        service.pair(10L, MAC, new Pair(CAB));
        assertThrows(RenException.class, () -> service.online(report().put("macAddress", "00:11:22:33:44:55")));
        assertThrows(RenException.class, () -> service.online(report().putNull("macAddress")));
        assertThrows(RenException.class, () -> service.online(report().put("cmd", "bindUser")));
        assertTrue(service.online(report().put("macAddress", "AA-BB-CC-DD-EE-FF")).fresh());
        assertTrue(service.online(report()).fresh());
    }

    @Test
    void onlineWhitelistsScalarsAndUsesOnlyServerReceiptTime() {
        service.pair(10L, MAC, new Pair(CAB));
        ObjectNode raw = report().put("bindingToken", "sensitive-token").put("tokenHash", "sensitive-hash")
                .put("ownerId", 20L).put("receivedAt", "2099-01-01T00:00:00Z")
                .put("unknown", "sensitive-unknown").put("Volume", 40);
        raw.putObject("profile").put("nickname", "spoofed");
        raw.putObject("Role").put("bindingToken", "nested-token");
        View view = service.online(raw);
        assertEquals(Instant.ofEpochMilli(now).toString(), view.receivedAt());
        assertEquals("75", view.report().get("Battery Level").textValue());
        assertEquals(40, view.report().get("Volume").intValue());
        for (String key : new String[]{"bindingToken", "tokenHash", "ownerId", "receivedAt", "unknown", "profile", "Role"}) {
            assertFalse(view.report().has(key), key);
            assertFalse(mapping.getReportJson().contains(key), key);
        }
        assertEquals(view, service.current(10L, MAC));
        time(now + 180_000);
        assertTrue(service.current(10L, MAC).fresh());
        time(now + 180_001);
        assertFalse(service.current(10L, MAC).fresh());
        time(now - 1);
        assertFalse(service.current(10L, MAC).fresh());
    }

    @Test
    void missingStaleFutureOrDifferentBootNeverCreatesBinding() {
        assertThrows(RenException.class, () -> service.legacy(request("r")));
        service.pair(10L, MAC, new Pair(CAB));
        assertThrows(RenException.class, () -> service.legacy(request("r")));
        service.online(report());
        assertThrows(RenException.class, () -> service.legacy(new Legacy(CAB, "other", "user-1", "r")));
        time(now + 180_001);
        assertThrows(RenException.class, () -> service.legacy(request("r")));
        time(now - 1);
        assertThrows(RenException.class, () -> service.legacy(request("r")));
        verify(bindings, never()).create(any(), any(), any());
    }

    @Test
    void positiveLegacyFlowUsesAuthoritativeOwnerAndProfileAndHashOnlySession() {
        observed();
        Binding issued = service.legacy(request("r1"));
        verify(bindings).create(10L, MAC, new Create("boot-1", "user-1", null));
        assertEquals("Authoritative profile", issued.profile().nickname());
        assertEquals("bindUser", issued.command().cmd());
        assertTrue(BindingTokens.matches(issued.bindingToken(), sessions.get(issued.usageSessionId()).getTokenHash()));
        String ciphertext = outbox.get(CAB + ":r1").getEncryptedBinding();
        assertTrue(ciphertext.startsWith("v1."));
        assertFalse(ciphertext.contains(issued.bindingToken()));
        assertFalse(service.current(10L, MAC).toString().contains(issued.bindingToken()));
        verify(devices, never()).upsertProfile(any());
    }

    @Test
    void exactRetrySurvivesServiceRestartAndNewIdReusesCommandWithoutExtendingExpiry() {
        observed();
        Binding first = service.legacy(request("r1"));
        service = new MqttBindingService(dao, devices, bindings, cipher, json);
        time(now + 1000);
        assertEquals(first, service.legacy(request("r1")));
        Binding alias = service.legacy(request("r2"));
        assertEquals(first, alias);
        assertEquals(outbox.get(CAB + ":r1").getExpiresAt(), outbox.get(CAB + ":r2").getExpiresAt());
        assertNotEquals(outbox.get(CAB + ":r1").getEncryptedBinding(), outbox.get(CAB + ":r2").getEncryptedBinding());
        verify(bindings, times(1)).create(any(), any(), any());
        verify(devices, times(1)).insertSession(any());
    }

    @Test
    void reusedRequestIdWithDifferentUserOrBootIsRejected() {
        observed();
        service.legacy(request("r1"));
        assertThrows(RenException.class, () -> service.legacy(new Legacy(CAB, "boot-1", "user-2", "r1")));
        service.online(report().put("bootID", "boot-2"));
        assertThrows(RenException.class, () -> service.legacy(new Legacy(CAB, "boot-2", "user-1", "r1")));
        verify(bindings, times(1)).create(any(), any(), any());
    }

    @Test
    void revokedAndSupersededRetriesNeverRotateCurrentBinding() {
        observed();
        Binding first = service.legacy(request("r1"));
        bindings.revoke(10L, MAC);
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        Binding second = service.legacy(request("r2"));
        assertNotEquals(first.usageSessionId(), second.usageSessionId());
        Binding newer = service.legacy(new Legacy(CAB, "boot-1", "user-2", "r3"));
        assertThrows(RenException.class, () -> service.legacy(request("r2")));
        assertEquals(newer.usageSessionId(), bindings.current(10L, MAC).usageSessionId());
        verify(bindings, times(3)).create(any(), any(), any());
    }

    @Test
    void expiredOutboxIsTombstoneButNewIntentionalIdRecoversUndeliveredBinding() {
        observed();
        Binding first = service.legacy(request("r1"));
        time(now + 180_000);
        service.online(report());
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        outbox.get(CAB + ":r1").setEncryptedBinding(null);
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        Binding recovered = service.legacy(request("r2"));
        assertNotEquals(first.usageSessionId(), recovered.usageSessionId());
        assertEquals(first.bindingVersion() + 1, recovered.bindingVersion());
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        assertEquals(recovered, service.legacy(request("r2")));
        assertEquals(2, outbox.size());
        verify(bindings, times(2)).create(any(), any(), any());
    }

    @Test
    void rebootInvalidatesCachedCommandEvenIfOldBootIsObservedAgain() {
        observed();
        service.legacy(request("r1"));
        service.online(report().put("bootID", "boot-2"));
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        service.online(report());
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        verify(bindings, times(1)).create(any(), any(), any());
    }

    @Test
    void ownerOrRegisteredDeviceIdentityChangeInvalidatesMappingUntilExplicitRepair() {
        observed();
        service.legacy(request("r1"));
        device.setUserId(20L);
        assertNull(service.current(20L, MAC).cabGuid());
        assertThrows(RenException.class, () -> service.online(report()));
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        device.setUserId(10L);
        device.setId("replacement-device");
        assertThrows(RenException.class, () -> service.online(report()));
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        service.pair(10L, MAC, new Pair(CAB));
        assertNull(service.current(10L, MAC).report());
        service.online(report());
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        assertNotNull(service.legacy(request("r2")));
    }

    @Test
    void retryRevalidatesAgentOwnershipAndSessionExpiry() {
        observed();
        Binding issued = service.legacy(request("r1"));
        agent.setUserId(20L);
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        agent.setUserId(10L);
        sessions.get(issued.usageSessionId()).setExpiresAt(new Date(0));
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        verify(bindings, times(1)).create(any(), any(), any());
    }

    @Test
    void lockOrderIsDeviceThenMappingThenBindingState() {
        observed();
        clearInvocations(dao, devices);
        service.legacy(request("r1"));
        var ordered = inOrder(dao, devices);
        ordered.verify(dao).byCab(CAB);
        ordered.verify(devices).lockDevice(MAC);
        ordered.verify(dao).lockMapping(MAC);
        ordered.verify(devices).lockDevice(MAC);
        ordered.verify(devices).ensureState(MAC);
        ordered.verify(devices).lockState(MAC);
        ordered.verify(dao).request(MAC, CAB, "r1");
    }

    @Test
    void activeSameBootUserWithoutCiphertextCanRecoverWithNewIntentionalId() {
        observed();
        Binding manuallyIssued = bindings.create(10L, MAC, new Create("boot-1", "user-1", null));
        Binding recovered = service.legacy(request("r1"));
        assertNotEquals(manuallyIssued.usageSessionId(), recovered.usageSessionId());
        assertEquals(recovered.usageSessionId(), bindings.current(10L, MAC).usageSessionId());
        verify(devices, times(2)).insertSession(any());
    }

    @Test
    void matchingReadyObservationPreventsNewIdFromRotatingAfterCacheExpiry() {
        observed();
        Binding issued = service.legacy(request("r1"));
        time(now + 180_000);
        service.online(report().put("bindingState", "ready").put("usageSessionID", issued.usageSessionId())
                .put("bindingVersion", issued.bindingVersion()));
        assertThrows(RenException.class, () -> service.legacy(request("r1")));
        assertThrows(RenException.class, () -> service.legacy(request("r2")));
        verify(bindings, times(1)).create(any(), any(), any());
        service.online(report().put("bindingState", "ready").put("usageSessionID", "older-session")
                .put("bindingVersion", issued.bindingVersion()));
        assertNotEquals(issued.usageSessionId(), service.legacy(request("r2")).usageSessionId());
    }

    @Test
    void cleanupScrubsCiphertextWithoutDeletingRequestIdentity() {
        service.purgeExpiredCommands();
        verify(dao).purgeExpiredCommands(new Date(now));
    }
}
