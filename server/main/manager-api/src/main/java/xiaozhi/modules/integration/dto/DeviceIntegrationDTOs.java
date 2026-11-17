package xiaozhi.modules.integration.dto;

import java.net.URI;
import java.util.Date;
import java.util.List;

import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.Size;
import xiaozhi.common.exception.RenException;
import xiaozhi.modules.agent.entity.AgentChatHistoryEntity;

public final class DeviceIntegrationDTOs {
    private DeviceIntegrationDTOs() {}

    public record Media(@Size(max=2048) String imageUrl, @Size(max=2048) String videoUrl,
                        @Size(max=2048) String audioUrl) {}
    public record SelectRole(@NotBlank @Size(max=32) String agentId,
                             @NotBlank @Size(max=64) String roleId) {}
    public record Role(String agentId, String roleId, String roleName, String roleDefinition,
                       String voiceId, String voiceName, String voiceCode, String imageUrl,
                       String videoUrl, String audioUrl, Integer sort, boolean enabled) {}
    public record RoleBinding(String deviceId, String macAddress, String agentId, String roleId,
                              Long bindingVersion, Date updatedAt) {}
    public record BindingResult(RoleBinding binding, String runtimeStatus) {}
    public record Cleared(String deviceId, String macAddress, long deletedCount, Date clearedAt,
                          String runtimeStatus) {}
    public record Message(String id, String agentId, String roleId, String userGUID,
                          String sessionId, String usageSessionID, String turnId, Byte chatType,
                          String content, String audioId, String deliveryStatus, Date createdAt) {
        public static Message from(AgentChatHistoryEntity e) {
            return new Message(e.getId().toString(), e.getAgentId(), e.getRoleId(), e.getUserGuid(),
                    e.getSessionId(), e.getUsageSessionId(), e.getTurnId(), e.getChatType(),
                    e.getContent(), e.getAudioId(), e.getDeliveryStatus(), e.getCreatedAt());
        }
    }
    public record History(String deviceId, String macAddress, int page, int limit, long total,
                          List<Message> list) {}

    public static String mediaUrl(String value) {
        if (value == null || value.isBlank()) return null;
        try {
            URI uri = URI.create(value);
            if (value.length() > 2048 || !value.equals(value.trim()) || uri.getHost() == null
                    || uri.getRawUserInfo() != null
                    || !("https".equalsIgnoreCase(uri.getScheme()) || "http".equalsIgnoreCase(uri.getScheme()))) {
                throw new IllegalArgumentException();
            }
            return value;
        } catch (IllegalArgumentException e) {
            throw new RenException("Media URL must be an absolute HTTP or HTTPS URL without credentials");
        }
    }
}
