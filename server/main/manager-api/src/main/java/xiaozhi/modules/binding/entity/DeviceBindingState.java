package xiaozhi.modules.binding.entity;

import lombok.Data;

@Data
public class DeviceBindingState {
    private String macAddress;
    private Long bindingVersion;
    private String activeSessionId;
}
