package xiaozhi.modules.binding.entity;

import lombok.Data;

@Data
public class UserProfileEntity {
    private Long ownerId;
    private String userGuid;
    private String nickname;
    private String language;
    private String interests;
    private String explanationStyle;
    private String relationship;
    private Integer usageCount;
    private Long profileVersion;
}
