package xiaozhi.modules.config.dto;

import java.io.Serializable;

import io.swagger.v3.oas.annotations.media.Schema;
import lombok.Data;

@Data
@Schema(description = "Enabled agent role and voice binding")
public class AgentRoleBindingResponseDTO implements Serializable {
    private static final long serialVersionUID = 1L;

    @Schema(description = "Agent ID")
    private String agentId;

    @Schema(description = "Role ID")
    private String roleId;

    @Schema(description = "Role name")
    private String roleName;

    @Schema(description = "Role definition")
    private String roleDefinition;

    @Schema(description = "Voice record ID")
    private String voiceId;

    @Schema(description = "Voice display name")
    private String voiceName;

    @Schema(description = "Voice code used by TTS")
    private String voiceCode;

    @Schema(description = "Sort order")
    private Integer sort;
}
