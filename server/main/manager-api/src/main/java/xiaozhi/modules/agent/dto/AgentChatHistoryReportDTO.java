package xiaozhi.modules.agent.dto;

import io.swagger.v3.oas.annotations.media.Schema;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Size;
import jakarta.validation.constraints.Pattern;
import jakarta.validation.constraints.Positive;
import jakarta.validation.constraints.AssertTrue;
import com.fasterxml.jackson.annotation.JsonAlias;
import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.databind.annotation.JsonSerialize;
import lombok.Data;
import xiaozhi.modules.binding.json.ProtocolNumberSerializer;

/**
 * 小智设备聊天上报请求
 *
 * @author Haotian
 * @version 1.0, 2025/5/8
 */
@Data
@Schema(description = "小智设备聊天上报请求")
public class AgentChatHistoryReportDTO {
    @Schema(description = "MAC地址", example = "00:11:22:33:44:55")
    @NotBlank
    private String macAddress;
    @Schema(description = "userGUID for agent-scoped memory isolation", example = "DTkxHempvRmp")
    @JsonAlias("userGUID")
    private String userGuid;
    @Size(min = 1, max = 128)
    private String messageId;
    @Size(min = 1, max = 36)
    @JsonAlias("usageSessionID")
    private String usageSessionId;
    @Size(min = 1, max = 128)
    @JsonAlias("bootID")
    private String bootId;
    @Size(max = 128)
    private String roleId;
    @Size(max = 128)
    private String turnId;
    @Positive
    @JsonSerialize(using = ProtocolNumberSerializer.class)
    private Long bindingVersion;
    private Boolean textTruncated;
    @Pattern(regexp = "received|generated|sent|sent_unconfirmed|played|failed|interrupted")
    private String deliveryStatus;
    // Only the trusted reporting service may assert receipt of a real firmware playback ACK.
    private Boolean playbackAck;
    @Schema(description = "会话ID", example = "79578c31-f1fb-426a-900e-1e934215f05a")
    @NotBlank
    private String sessionId;
    @Schema(description = "消息类型: 1-用户, 2-智能体", example = "1")
    @NotNull
    private Byte chatType;
    @Schema(description = "聊天内容", example = "你好呀")
    @NotNull
    private String content;
    @Schema(description = "base64编码的opus音频数据", example = "")
    private String audioBase64;
    @Schema(description = "上报时间，十位时间戳，空时默认使用当前时间", example = "1745657732")
    @JsonSerialize(using = ProtocolNumberSerializer.class)
    private Long reportTime;
    @jakarta.validation.constraints.PositiveOrZero
    private Long historyGeneration;

    @JsonIgnore
    @AssertTrue(message = "content must be nonempty for bound reports and nonblank for legacy reports")
    public boolean isContentValid() {
        return content != null && (usageSessionId != null ? !content.isEmpty() : !content.isBlank());
    }
}
