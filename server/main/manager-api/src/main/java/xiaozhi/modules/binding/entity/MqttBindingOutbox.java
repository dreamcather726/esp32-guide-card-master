package xiaozhi.modules.binding.entity;

import java.util.Date;

import com.fasterxml.jackson.annotation.JsonIgnore;
import lombok.Data;
import lombok.ToString;

@Data
public class MqttBindingOutbox {
    private String macAddress;
    private String cabGuid;
    private String requestId;
    private String mappingId;
    private String bootId;
    private String userGuid;
    private String usageSessionId;
    private Long bindingVersion;
    private Date expiresAt;
    @JsonIgnore
    @ToString.Exclude
    private String encryptedBinding;
}
