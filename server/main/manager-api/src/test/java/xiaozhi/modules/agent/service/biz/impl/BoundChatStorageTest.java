package xiaozhi.modules.agent.service.biz.impl;

import static org.junit.jupiter.api.Assertions.*;
import static org.mockito.ArgumentMatchers.*;
import static org.mockito.Mockito.*;

import java.util.HashMap;
import java.util.Map;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.test.util.ReflectionTestUtils;

import xiaozhi.common.exception.RenException;
import xiaozhi.common.redis.RedisUtils;
import xiaozhi.modules.agent.dao.AiAgentChatHistoryDao;
import xiaozhi.modules.agent.dto.AgentChatHistoryReportDTO;
import xiaozhi.modules.agent.entity.AgentChatHistoryEntity;
import xiaozhi.modules.agent.entity.AgentEntity;
import xiaozhi.modules.agent.service.*;
import xiaozhi.modules.binding.entity.UsageSessionEntity;
import xiaozhi.modules.binding.service.UserBindingService;
import xiaozhi.modules.device.service.DeviceService;

class BoundChatStorageTest {
    private AgentService agents;
    private AgentChatHistoryService history;
    private AgentChatAudioService audio;
    private UserBindingService binding;
    private AiAgentChatHistoryDao dao;
    private AgentChatHistoryBizServiceImpl service;
    private final Map<String, AgentChatHistoryEntity> rows = new HashMap<>();

    @BeforeEach
    void setup() {
        rows.clear();
        agents = mock(AgentService.class);
        history = mock(AgentChatHistoryService.class);
        audio = mock(AgentChatAudioService.class);
        binding = mock(UserBindingService.class);
        dao = mock(AiAgentChatHistoryDao.class);
        service = new AgentChatHistoryBizServiceImpl(agents, history, audio,
                mock(AgentChatSummaryService.class), mock(RedisUtils.class), mock(DeviceService.class), binding, dao,
                mock(xiaozhi.modules.integration.service.DeviceConversationService.class));
        UsageSessionEntity session = new UsageSessionEntity();
        session.setId("usage-1");
        session.setMacAddress("aa:bb:cc:dd:ee:ff");
        session.setUserGuid("user-1");
        session.setBootId("boot-1");
        session.setAgentId("snapshot-agent");
        session.setAgentName("Original guide");
        session.setOwnerId(10L);
        session.setProfileSnapshot("{\"nickname\":\"Alice\"}");
        session.setProfileVersion(4L);
        session.setBindingVersion(3L);
        when(binding.validateReport(anyString(), anyString(), anyString(), anyString(), anyLong())).thenReturn(session);
        doAnswer(call -> {
            AgentChatHistoryEntity row = call.getArgument(0);
            row.setId(1L);
            rows.putIfAbsent(row.getMessageId(), row);
            return null;
        }).when(dao).insertIdempotent(any());
        when(dao.lockMessage(anyString())).thenAnswer(call -> rows.get(call.getArgument(0)));
    }

    private AgentChatHistoryReportDTO report() {
        AgentChatHistoryReportDTO report = new AgentChatHistoryReportDTO();
        report.setMacAddress("aa:bb:cc:dd:ee:ff");
        report.setUserGuid("user-1");
        report.setBootId("boot-1");
        report.setUsageSessionId("usage-1");
        report.setSessionId("connection-1");
        report.setMessageId("message-1");
        report.setBindingVersion(3L);
        report.setTurnId("turn-1");
        report.setRoleId("role-1");
        report.setChatType((byte) 2);
        report.setContent("Hello");
        report.setAudioBase64("YXVkaW8=");
        return report;
    }

    @Test
    void boundTextAlwaysPersistsAndUsesSnapshotsWithAudioOffByDefault() {
        assertTrue(service.report(report()));
        AgentChatHistoryEntity row = rows.get("message-1");
        assertEquals("snapshot-agent", row.getAgentId());
        assertEquals("Original guide", row.getAgentName());
        assertEquals("user-1", row.getUserGuid());
        assertEquals(10L, row.getOwnerId());
        assertEquals(4L, row.getProfileVersion());
        assertEquals("generated", row.getDeliveryStatus());
        verifyNoInteractions(agents, audio, history);
    }

    @Test
    void duplicateStableIdIsOneRowAndDeliveryNeverRegresses() {
        AgentChatHistoryReportDTO request = report();
        service.report(request);
        request.setDeliveryStatus("sent");
        service.report(request);
        request.setDeliveryStatus("generated");
        service.report(request);
        assertEquals(1, rows.size());
        assertEquals("sent", rows.get("message-1").getDeliveryStatus());
        verify(dao, times(1)).updateDelivery(1L, "sent");
    }

    @Test
    void changedContentForSameIdIsRejected() {
        AgentChatHistoryReportDTO request = report();
        service.report(request);
        request.setContent("different text");
        assertThrows(RenException.class, () -> service.report(request));
        assertEquals("Hello", rows.get("message-1").getContent());
    }

    @Test
    void playedRequiresRealAckFlagAndRemainsTerminal() {
        AgentChatHistoryReportDTO request = report();
        request.setDeliveryStatus("played");
        assertThrows(RenException.class, () -> service.report(request));
        assertTrue(rows.isEmpty());
        request.setPlaybackAck(true);
        service.report(request);
        request.setDeliveryStatus("sent");
        service.report(request);
        assertEquals("played", rows.get("message-1").getDeliveryStatus());
    }

    @Test
    void identityFailureRejectsBeforeWriting() {
        when(binding.validateReport(anyString(), anyString(), anyString(), anyString(), anyLong())).thenThrow(new RenException("mismatch"));
        assertThrows(RenException.class, () -> service.report(report()));
        verifyNoInteractions(dao, audio);
    }

    @Test
    void boundReportsRequireIdAndText() {
        AgentChatHistoryReportDTO request = report();
        request.setMessageId(null);
        assertThrows(RenException.class, () -> service.report(request));
        request.setMessageId("message-1");
        request.setContent("");
        assertThrows(RenException.class, () -> service.report(request));
        request.setContent("a".repeat(16001));
        assertThrows(RenException.class, () -> service.report(request));
        verifyNoInteractions(dao);
    }

    @Test
    void whitespaceChunksAreRetainedAndLegacyWhitespaceStillFailsValidation() {
        AgentChatHistoryReportDTO request = report();
        request.setContent(" \n\t ");
        try (var factory = jakarta.validation.Validation.buildDefaultValidatorFactory()) {
            assertTrue(factory.getValidator().validate(request).isEmpty());
            service.report(request);
            assertEquals(" \n\t ", rows.get("message-1").getContent());
            request.setUsageSessionId(null);
            assertFalse(factory.getValidator().validate(request).isEmpty());
        }
    }

    @Test
    void stableMessageIdSurvivesTransportReconnect() {
        AgentChatHistoryReportDTO request = report();
        service.report(request);
        request.setSessionId("connection-2");
        service.report(request);
        assertEquals(1, rows.size());
        assertEquals("connection-1", rows.get("message-1").getSessionId());
    }

    @Test
    void sentUnconfirmedAndTruncationAuditAreStoredWithoutImplyingPlayback() {
        AgentChatHistoryReportDTO request = report();
        request.setTextTruncated(true);
        service.report(request);
        request.setDeliveryStatus("sent_unconfirmed");
        service.report(request);
        AgentChatHistoryEntity row = rows.get("message-1");
        assertEquals("sent_unconfirmed", row.getDeliveryStatus());
        assertTrue(row.getTextTruncated());
        assertEquals(3L, row.getBindingVersion());
        verify(binding, times(2)).validateReport(request.getMacAddress(), "user-1", "boot-1", "usage-1", 3L);
    }

    @Test
    void secondsTimestampIsConvertedToMilliseconds() {
        AgentChatHistoryReportDTO request = report();
        request.setReportTime(1700000000L);
        service.report(request);
        assertEquals(1700000000000L, rows.get("message-1").getCreatedAt().getTime());
    }

    @Test
    void millisecondTimestampIsPreservedAndExposedAsReportTime() {
        AgentChatHistoryReportDTO request = report();
        request.setReportTime(1700000000123L);
        service.report(request);
        assertEquals(1700000000123L, rows.get("message-1").getReportTime());
    }

    @Test
    void rawAudioRequiresSeparateExplicitOptIn() {
        ReflectionTestUtils.setField(service, "storeBoundRawAudio", true);
        when(audio.saveAudio(any())).thenReturn("audio-1");
        service.report(report());
        verify(audio).saveAudio(any());
        verify(dao).attachAudio(1L, "audio-1");
    }

    @Test
    void legacyDisabledHistoryStillDoesNotRecord() {
        AgentChatHistoryReportDTO request = report();
        request.setUsageSessionId(null);
        request.setBootId(null);
        request.setMessageId(null);
        AgentEntity agent = new AgentEntity();
        agent.setId("legacy-agent");
        agent.setChatHistoryConf(0);
        when(agents.getDefaultAgentByMacAddress(anyString())).thenReturn(agent);
        assertTrue(service.report(request));
        verifyNoInteractions(history, audio, dao, binding);
    }

    @Test
    void legacyTextHistoryKeepsExistingSavePath() {
        AgentChatHistoryReportDTO request = report();
        request.setUsageSessionId(null);
        request.setBootId(null);
        request.setMessageId(null);
        AgentEntity agent = new AgentEntity();
        agent.setId("legacy-agent");
        agent.setChatHistoryConf(1);
        when(agents.getDefaultAgentByMacAddress(anyString())).thenReturn(agent);
        assertTrue(service.report(request));
        verify(history).save(any());
        verifyNoInteractions(dao, binding, audio);
    }

    @Test
    void failedOrInterruptedStatusDoesNotBecomeSentOnRetry() {
        assertFalse(AgentChatHistoryBizServiceImpl.shouldAdvanceDelivery("failed", "sent"));
        assertFalse(AgentChatHistoryBizServiceImpl.shouldAdvanceDelivery("interrupted", "generated"));
        assertTrue(AgentChatHistoryBizServiceImpl.shouldAdvanceDelivery("sent", "interrupted"));
        assertTrue(AgentChatHistoryBizServiceImpl.shouldAdvanceDelivery("sent", "played"));
    }
}
