package xiaozhi.modules.agent.entity;

import java.util.Date;

import com.baomidou.mybatisplus.annotation.IdType;
import com.baomidou.mybatisplus.annotation.TableField;
import com.baomidou.mybatisplus.annotation.TableId;
import com.baomidou.mybatisplus.annotation.TableName;
import com.fasterxml.jackson.databind.annotation.JsonSerialize;

import lombok.AllArgsConstructor;
import lombok.Builder;
import lombok.Data;
import lombok.NoArgsConstructor;
import xiaozhi.modules.binding.json.ProtocolNumberSerializer;

/**
 * 智能体聊天记录表
 *
 * @author Goody
 * @version 1.0, 2025/4/30
 * @since 1.0.0
 */
@Data
@Builder
@AllArgsConstructor
@NoArgsConstructor
@TableName(value = "ai_agent_chat_history")
public class AgentChatHistoryEntity {
    /**
     * 主键ID
     */
    @TableId(type = IdType.AUTO)
    private Long id;

    /**
     * MAC地址
     */
    @TableField(value = "mac_address")
    private String macAddress;

    /**
     * 智能体id
     */
    @TableField(value = "agent_id")
    private String agentId;

    @TableField(value = "user_guid")
    private String userGuid;

    private String messageId;
    private String usageSessionId;
    private String bootId;
    private String roleId;
    private String turnId;
    private String deliveryStatus;
    @JsonSerialize(using = ProtocolNumberSerializer.class)
    private Long bindingVersion;
    private Boolean textTruncated;
    private Long ownerId;
    private String agentName;
    private String profileSnapshot;
    @JsonSerialize(using = ProtocolNumberSerializer.class)
    private Long profileVersion;

    /**
     * 会话ID
     */
    @TableField(value = "session_id")
    private String sessionId;

    /**
     * 消息类型: 1-用户, 2-智能体
     */
    @TableField(value = "chat_type")
    private Byte chatType;

    /**
     * 聊天内容
     */
    @TableField(value = "content")
    private String content;

    /**
     * 音频base64数据
     */
    @TableField(value = "audio_id")
    private String audioId;

    /**
     * 创建时间
     */
    @TableField(value = "created_at")
    private Date createdAt;

    /**
     * 更新时间
     */
    @TableField(value = "updated_at")
    private Date updatedAt;

    @JsonSerialize(using = ProtocolNumberSerializer.class)
    public Long getReportTime() {
        return createdAt == null ? null : createdAt.getTime();
    }
}
