package xiaozhi.modules.binding.dto;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;
import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.annotation.JsonProperty;
import com.fasterxml.jackson.databind.JsonNode;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.Size;

public final class MqttBindingDTOs {
    private MqttBindingDTOs() {
    }

    public record Pair(@NotBlank @Size(max = 128) String cabGuid) {
    }

    @JsonIgnoreProperties(ignoreUnknown = true)
    public record Legacy(@NotBlank @Size(max = 128) String cabGuid,
            @JsonProperty("bootID") @NotBlank @Size(max = 128) String bootId,
            @JsonProperty("userGUID") @NotBlank @Size(max = 128) String userGuid,
            @JsonProperty("requestID") @NotBlank @Size(max = 128) String requestId) {
    }

    @JsonInclude(JsonInclude.Include.ALWAYS)
    public record View(String macAddress, String cabGuid, JsonNode report, String receivedAt, boolean fresh) {
    }
}
