package xiaozhi.modules.binding.dto;

import java.util.Date;

import com.fasterxml.jackson.annotation.JsonAlias;
import com.fasterxml.jackson.annotation.JsonIgnoreProperties;
import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.annotation.JsonProperty;
import com.fasterxml.jackson.databind.annotation.JsonSerialize;
import com.fasterxml.jackson.databind.annotation.JsonDeserialize;
import jakarta.validation.Valid;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.Positive;
import jakarta.validation.constraints.Size;
import xiaozhi.modules.binding.json.ProtocolNumberSerializer;
import xiaozhi.modules.binding.json.ProfileUsageCountDeserializer;

public final class UserBindingDTOs {
    private UserBindingDTOs() {
    }

    @JsonIgnoreProperties(ignoreUnknown = true)
    public record Profile(
            @Size(max = 64) String nickname,
            @Size(max = 32) String language,
            @Size(max = 1024) String interests,
            @Size(max = 512) String explanationStyle,
            @Size(max = 64) String relationship,
            @Min(0) @JsonDeserialize(using = ProfileUsageCountDeserializer.class) Integer usageCount) {
        public Profile(String nickname, String language, String interests, String explanationStyle) {
            this(nickname, language, interests, explanationStyle, null, null);
        }
    }

    public record ProfileView(@JsonProperty("userGUID") String userGuid,
            @JsonSerialize(using = ProtocolNumberSerializer.class) long profileVersion, Profile profile) {
    }

    public record Create(
            @JsonProperty("bootID") @JsonAlias("bootId") @NotBlank @Size(max = 128) String bootId,
            @JsonProperty("userGUID") @JsonAlias("userGuid") @NotBlank @Size(max = 128) String userGuid,
            @Valid Profile profile) {
    }

    public record Resolve(
            @NotBlank @Size(max = 50) String macAddress,
            @JsonProperty("userGUID") @JsonAlias("userGuid") @NotBlank @Size(max = 128) String userGuid,
            @JsonProperty("bootID") @JsonAlias("bootId") @NotBlank @Size(max = 128) String bootId,
            @NotNull @Positive @JsonSerialize(using = ProtocolNumberSerializer.class) Long bindingVersion,
            @NotBlank @Size(min = 43, max = 128) String bindingToken) {
        @Override
        public String toString() {
            return "Resolve[redacted]";
        }
    }

    @JsonInclude(JsonInclude.Include.NON_NULL)
    public record Command(String cmd, String cmdID, @JsonProperty("userGUID") String userGuid,
            @JsonProperty("bootID") String bootId,
            @JsonSerialize(using = ProtocolNumberSerializer.class) long bindingVersion,
            @JsonProperty("usageSessionID") String usageSessionId, String bindingToken) {
        @Override
        public String toString() {
            return "Command[redacted]";
        }
    }

    @JsonInclude(JsonInclude.Include.NON_NULL)
    public record Binding(@JsonProperty("userGUID") String userGuid,
            @JsonProperty("bootID") String bootId,
            @JsonSerialize(using = ProtocolNumberSerializer.class) long bindingVersion,
            @JsonProperty("usageSessionID") String usageSessionId, String bindingToken,
            Profile profile, @JsonSerialize(using = ProtocolNumberSerializer.class) long profileVersion,
            String agentId, Date expiresAt, Command command) {
        @Override
        public String toString() {
            return "Binding[redacted]";
        }
    }
}
