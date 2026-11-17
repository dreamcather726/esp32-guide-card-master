package xiaozhi.modules.agent.service.biz.impl;

import java.util.Base64;
import java.util.Date;
import java.util.Objects;

import org.apache.commons.lang3.StringUtils;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;
import org.springframework.beans.factory.annotation.Value;

import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import xiaozhi.common.constant.Constant;
import xiaozhi.common.exception.RenException;
import xiaozhi.modules.agent.dao.AiAgentChatHistoryDao;
import xiaozhi.modules.binding.entity.UsageSessionEntity;
import xiaozhi.modules.binding.service.UserBindingService;
import xiaozhi.common.redis.RedisKeys;
import xiaozhi.common.redis.RedisUtils;
import xiaozhi.modules.agent.dto.AgentChatHistoryReportDTO;
import xiaozhi.modules.agent.entity.AgentChatHistoryEntity;
import xiaozhi.modules.agent.entity.AgentEntity;
import xiaozhi.modules.agent.service.AgentChatAudioService;
import xiaozhi.modules.agent.service.AgentChatHistoryService;
import xiaozhi.modules.agent.service.AgentChatSummaryService;
import xiaozhi.modules.agent.service.AgentService;
import xiaozhi.modules.agent.service.biz.AgentChatHistoryBizService;
import xiaozhi.modules.device.entity.DeviceEntity;
import xiaozhi.modules.device.service.DeviceService;
import xiaozhi.modules.integration.service.DeviceConversationService;

/**
 * {@link AgentChatHistoryBizService} impl
 *
 * @author Goody
 * @version 1.0, 2025/4/30
 * @since 1.0.0
 */
@Service
@Slf4j
@RequiredArgsConstructor
public class AgentChatHistoryBizServiceImpl implements AgentChatHistoryBizService {
    private final AgentService agentService;
    private final AgentChatHistoryService agentChatHistoryService;
    private final AgentChatAudioService agentChatAudioService;
    private final AgentChatSummaryService agentChatSummaryService;
    private final RedisUtils redisUtils;
    private final DeviceService deviceService;
    private final UserBindingService userBindingService;
    private final AiAgentChatHistoryDao chatHistoryDao;
    private final DeviceConversationService deviceConversations;

    @Value("${user-binding.store-raw-audio:false}")
    private boolean storeBoundRawAudio;

    /**
     * 处理聊天记录上报，包括文件上传和相关信息记录
     *
     * @param report 包含聊天上报所需信息的输入对象
     * @return 上传结果，true表示成功，false表示失败
     */
    @Override
    @Transactional(rollbackFor = Exception.class)
    public Boolean report(AgentChatHistoryReportDTO report) {
        // Serialize against per-device clears, before taking the usage-session lock.
        DeviceEntity reportingDevice = deviceConversations.lockForReport(report.getMacAddress());
        if (report.getUsageSessionId() != null) {
            return reportBound(report);
        }
        if (report.getBootId() != null) {
            throw new RenException("usageSessionId is required for bound reports");
        }
        validateDelivery(report);
        if (reportingDevice != null && deviceConversations.discardedReport(reportingDevice.getUserId(),
                reportingDevice.getMacAddress(), report.getReportTime(), report.getMessageId(), report.getHistoryGeneration())) return Boolean.TRUE;
        String macAddress = report.getMacAddress();
        Byte chatType = report.getChatType();
        Long reportTimeMillis = null != report.getReportTime() ? report.getReportTime()
                : System.currentTimeMillis();
        log.info("小智设备聊天上报请求: macAddress={}, type={} reportTime={}", macAddress, chatType, reportTimeMillis);

        // 根据设备MAC地址查询对应的默认智能体，判断是否需要上报
        AgentEntity agentEntity = agentService.getDefaultAgentByMacAddress(macAddress);
        if (agentEntity == null) {
            return Boolean.FALSE;
        }

        Integer chatHistoryConf = agentEntity.getChatHistoryConf();
        String agentId = agentEntity.getId();

        if (Objects.equals(chatHistoryConf, Constant.ChatHistoryConfEnum.RECORD_TEXT.getCode())) {
            saveChatText(report, agentId, macAddress, null, reportTimeMillis);
        } else if (Objects.equals(chatHistoryConf, Constant.ChatHistoryConfEnum.RECORD_TEXT_AUDIO.getCode())) {
            String audioId = report.getMessageId() == null ? saveChatAudio(report) : null;
            saveChatText(report, agentId, macAddress, audioId, reportTimeMillis);
            if (report.getMessageId() != null) {
                attachAudio(report, chatHistoryDao.lockMessage(report.getMessageId()));
            }
        }

        // 更新设备最后对话时间
        redisUtils.set(RedisKeys.getAgentDeviceLastConnectedAtById(agentId), new Date());

        // 更新设备最后连接时间
        DeviceEntity device = deviceService.getDeviceByMacAddress(macAddress);
        if (device != null) {
            deviceService.updateDeviceConnectionInfo(agentId, device.getId(), null);
        } else {
            log.warn("聊天记录上报时，未找到mac地址为 {} 的设备", macAddress);
        }

        return Boolean.TRUE;
    }

    /**
     * base64解码report.getOpusDataBase64(),存入ai_agent_chat_audio表
     */
    private String saveChatAudio(AgentChatHistoryReportDTO report) {
        String audioId = null;

        if (report.getAudioBase64() != null && !report.getAudioBase64().isEmpty()) {
            try {
                byte[] audioData = Base64.getDecoder().decode(report.getAudioBase64());
                audioId = agentChatAudioService.saveAudio(audioData);
                log.info("音频数据保存成功，audioId={}", audioId);
            } catch (Exception e) {
                log.error("音频数据保存失败", e);
                return null;
            }
        }
        return audioId;
    }

    /**
     * 组装上报数据
     */
    private void saveChatText(AgentChatHistoryReportDTO report, String agentId, String macAddress, String audioId,
            Long reportTime) {
        // 构建聊天记录实体
        AgentChatHistoryEntity entity = AgentChatHistoryEntity.builder()
                .macAddress(macAddress)
                .agentId(agentId)
                .userGuid(StringUtils.trimToNull(report.getUserGuid()))
                .messageId(report.getMessageId())
                .roleId(report.getRoleId())
                .turnId(report.getTurnId())
                .deliveryStatus(report.getDeliveryStatus())
                .textTruncated(report.getTextTruncated())
                .sessionId(report.getSessionId())
                .chatType(report.getChatType())
                .content(report.getContent())
                .audioId(audioId)
                .createdAt(new Date(reportTime))
                // NOTE(haotian): 2025/5/26 updateAt可以不设置，重点是createAt，而且这样可以看到上报延迟
                .build();

        // 保存数据
        if (report.getMessageId() == null) {
            agentChatHistoryService.save(entity);
        } else {
            saveIdempotent(entity);
        }

        log.info("设备 {} 对应智能体 {} 上报成功", macAddress, agentId);
    }

    private Boolean reportBound(AgentChatHistoryReportDTO report) {
        if (StringUtils.isBlank(report.getMessageId()) || report.getMessageId().length() > 128
                || StringUtils.isEmpty(report.getContent()) || report.getContent().length() > 16000
                || (!Objects.equals(report.getChatType(), (byte) 1) && !Objects.equals(report.getChatType(), (byte) 2))) {
            throw new RenException("Bound reports require messageId, text (max 16000), and chatType 1 or 2");
        }
        validateDelivery(report);
        UsageSessionEntity session = userBindingService.validateReport(report.getMacAddress(), report.getUserGuid(),
                report.getBootId(), report.getUsageSessionId(), report.getBindingVersion());
        if (deviceConversations.discardedReport(session.getOwnerId(), session.getMacAddress(),
                report.getReportTime(), report.getMessageId(), report.getHistoryGeneration())) return Boolean.TRUE;
        String status = report.getDeliveryStatus();
        if (status == null) {
            status = Objects.equals(report.getChatType(), (byte) 1) ? "received" : "generated";
        }
        Long timestamp = report.getReportTime();
        // Accept the documented seconds and the legacy Python millisecond representation.
        long millis = timestamp == null ? System.currentTimeMillis()
                : timestamp < 100000000000L ? Math.multiplyExact(timestamp, 1000) : timestamp;
        AgentChatHistoryEntity entity = AgentChatHistoryEntity.builder()
                .macAddress(session.getMacAddress()).agentId(session.getAgentId()).agentName(session.getAgentName())
                .ownerId(session.getOwnerId()).userGuid(session.getUserGuid()).sessionId(report.getSessionId())
                .messageId(report.getMessageId()).usageSessionId(session.getId()).bootId(session.getBootId())
                .roleId(report.getRoleId()).turnId(report.getTurnId()).deliveryStatus(status)
                .bindingVersion(session.getBindingVersion()).textTruncated(report.getTextTruncated())
                .profileSnapshot(session.getProfileSnapshot()).profileVersion(session.getProfileVersion())
                .chatType(report.getChatType()).content(report.getContent()).createdAt(new Date(millis)).build();
        AgentChatHistoryEntity saved = saveIdempotent(entity);
        if (storeBoundRawAudio) {
            attachAudio(report, saved);
        }
        return Boolean.TRUE;
    }

    private AgentChatHistoryEntity saveIdempotent(AgentChatHistoryEntity entity) {
        chatHistoryDao.insertIdempotent(entity);
        AgentChatHistoryEntity saved = chatHistoryDao.lockMessage(entity.getMessageId());
        // An ID collision must never overwrite another user's message or silently accept altered text.
        if (saved == null || !Objects.equals(saved.getMacAddress(), entity.getMacAddress())
                || !Objects.equals(saved.getAgentId(), entity.getAgentId())
                || !Objects.equals(saved.getUserGuid(), entity.getUserGuid())
                || (entity.getUsageSessionId() == null && !Objects.equals(saved.getSessionId(), entity.getSessionId()))
                || !Objects.equals(saved.getUsageSessionId(), entity.getUsageSessionId())
                || !Objects.equals(saved.getBootId(), entity.getBootId())
                || !Objects.equals(saved.getBindingVersion(), entity.getBindingVersion())
                || !Objects.equals(saved.getTextTruncated(), entity.getTextTruncated())
                || !Objects.equals(saved.getRoleId(), entity.getRoleId())
                || !Objects.equals(saved.getTurnId(), entity.getTurnId())
                || !Objects.equals(saved.getChatType(), entity.getChatType())
                || !Objects.equals(saved.getContent(), entity.getContent())) {
            throw new RenException("messageId is already associated with a different message");
        }
        String next = entity.getDeliveryStatus();
        if (next != null && shouldAdvanceDelivery(saved.getDeliveryStatus(), next)) {
            chatHistoryDao.updateDelivery(saved.getId(), next);
            saved.setDeliveryStatus(next);
        }
        return saved;
    }

    private void attachAudio(AgentChatHistoryReportDTO report, AgentChatHistoryEntity saved) {
        if (saved != null && saved.getAudioId() == null) {
            String audio = saveChatAudio(report);
            if (audio != null) {
                chatHistoryDao.attachAudio(saved.getId(), audio);
            }
        }
    }

    private void validateDelivery(AgentChatHistoryReportDTO report) {
        String status = report.getDeliveryStatus();
        if (status != null && !java.util.Set.of("received", "generated", "sent", "sent_unconfirmed", "played", "failed", "interrupted").contains(status)) {
            throw new RenException("Invalid deliveryStatus");
        }
        if ("played".equals(status) && !Boolean.TRUE.equals(report.getPlaybackAck())) {
            throw new RenException("played requires an explicit firmware playback ACK");
        }
        if (report.getMessageId() != null && StringUtils.isBlank(report.getMessageId())) {
            throw new RenException("Invalid messageId");
        }
    }

    static boolean shouldAdvanceDelivery(String current, String next) {
        if (Objects.equals(current, next) || "played".equals(current)) {
            return false;
        }
        if ("played".equals(next)) {
            return true;
        }
        if ("failed".equals(current) || "interrupted".equals(current)) {
            return false;
        }
        if ("failed".equals(next) || "interrupted".equals(next)) {
            return true;
        }
        return deliveryRank(next) > deliveryRank(current);
    }

    private static int deliveryRank(String status) {
        if (status == null) {
            return -1;
        }
        return switch (status) {
            case "received", "generated" -> 0;
            case "sent", "sent_unconfirmed" -> 1;
            default -> -1;
        };
    }
}
