package xiaozhi.modules.binding.entity;

import java.util.Date;

import lombok.Data;

@Data
public class MqttBindingMapping {
    private String macAddress;
    private String cabGuid;
    private String mappingId;
    private String deviceId;
    private Long ownerId;
    private String bootId;
    private String reportJson;
    private Date receivedAt;
}
