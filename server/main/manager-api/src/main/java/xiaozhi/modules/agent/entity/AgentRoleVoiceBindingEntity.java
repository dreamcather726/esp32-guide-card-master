package xiaozhi.modules.agent.entity;

import java.util.Date;

import com.baomidou.mybatisplus.annotation.IdType;
import com.baomidou.mybatisplus.annotation.TableId;
import com.baomidou.mybatisplus.annotation.TableName;

import io.swagger.v3.oas.annotations.media.Schema;
import lombok.Data;

@Data
@TableName("ai_agent_role_voice_binding")
@Schema(description = "Agent role and voice binding")
public class AgentRoleVoiceBindingEntity {

    @TableId(type = IdType.ASSIGN_UUID)
    @Schema(description = "Binding ID")
    private String id;

    @Schema(description = "Agent ID")
    private String agentId;

    @Schema(description = "Binding name")
    private String name;

    @Schema(description = "System prompt")
    private String systemPrompt;

    @Schema(description = "TTS voice ID")
    private String ttsVoiceId;

    private String imageUrl;
    private String videoUrl;
    private String audioUrl;

    @Schema(description = "Sort order")
    private Integer sort;

    @Schema(description = "Enabled")
    private Boolean enabled;

    @Schema(description = "Creator")
    private Long creator;

    @Schema(description = "Created time")
    private Date createdAt;

    @Schema(description = "Updater")
    private Long updater;

    @Schema(description = "Updated time")
    private Date updatedAt;
}
