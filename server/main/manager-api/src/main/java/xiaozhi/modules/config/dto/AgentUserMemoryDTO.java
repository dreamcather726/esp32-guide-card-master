package xiaozhi.modules.config.dto;

import io.swagger.v3.oas.annotations.media.Schema;
import jakarta.validation.constraints.NotBlank;
import lombok.Data;

@Data
@Schema(description = "Agent user memory request")
public class AgentUserMemoryDTO {
    @NotBlank
    private String agentId;

    @NotBlank
    private String userGuid;
}
