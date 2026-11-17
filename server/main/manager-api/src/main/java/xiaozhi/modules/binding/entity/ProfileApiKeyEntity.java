package xiaozhi.modules.binding.entity;

import lombok.Data;

@Data
public class ProfileApiKeyEntity {
    private Long ownerId;
    private String secretHash;
    private String scopes;
}
