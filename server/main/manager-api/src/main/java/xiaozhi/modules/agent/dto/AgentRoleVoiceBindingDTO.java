package xiaozhi.modules.agent.dto;

import java.io.Serializable;

import io.swagger.v3.oas.annotations.media.Schema;
import lombok.Data;

@Data
@Schema(description = "Agent role and voice binding")
public class AgentRoleVoiceBindingDTO implements Serializable {
    private static final long serialVersionUID = 1L;

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
}
