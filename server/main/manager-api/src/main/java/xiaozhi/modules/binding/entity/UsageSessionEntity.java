package xiaozhi.modules.binding.entity;

import java.util.Date;

import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.databind.annotation.JsonSerialize;
import lombok.Data;
import lombok.ToString;
import xiaozhi.modules.binding.json.ProtocolNumberSerializer;

@Data
public class UsageSessionEntity {
    private String id;
    private String macAddress;
    private String deviceId;
    private Long ownerId;
    private String userGuid;
    private String bootId;
    @JsonSerialize(using = ProtocolNumberSerializer.class)
    private Long bindingVersion;
    private String agentId;
    private String agentName;
    private String profileSnapshot;
    @JsonSerialize(using = ProtocolNumberSerializer.class)
    private Long profileVersion;
    @JsonIgnore
    @ToString.Exclude
    private String tokenHash;
    private Date expiresAt;
    private Date revokedAt;
    private Date createdAt;
}
