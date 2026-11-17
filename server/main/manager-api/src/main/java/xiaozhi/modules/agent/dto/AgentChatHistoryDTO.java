package xiaozhi.modules.agent.dto;

import java.util.Date;
import com.fasterxml.jackson.databind.annotation.JsonSerialize;

import io.swagger.v3.oas.annotations.media.Schema;
import lombok.Data;
import xiaozhi.modules.binding.json.ProtocolNumberSerializer;

/**
 * 智能体聊天记录DTO
 */
@Data
@Schema(description = "智能体聊天记录")
public class AgentChatHistoryDTO {
    private String messageId;
    private String usageSessionId;
    private String userGuid;
    private String bootId;
    private String roleId;
    private String turnId;
    private String deliveryStatus;
    @JsonSerialize(using = ProtocolNumberSerializer.class)
    private Long bindingVersion;
    private Boolean textTruncated;
    private String agentId;
    private String agentName;
    private String profileSnapshot;
    @JsonSerialize(using = ProtocolNumberSerializer.class)
    private Long profileVersion;
    @Schema(description = "创建时间")
    private Date createdAt;

    @Schema(description = "消息类型: 1-用户, 2-智能体")
    private Byte chatType;

    @Schema(description = "聊天内容")
    private String content;

    @Schema(description = "音频ID")
    private String audioId;

    @Schema(description = "MAC地址")
    private String macAddress;

    @JsonSerialize(using = ProtocolNumberSerializer.class)
    public Long getReportTime() {
        return createdAt == null ? null : createdAt.getTime();
    }
}
