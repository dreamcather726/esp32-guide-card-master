package xiaozhi.modules.binding.dao;

import org.apache.ibatis.annotations.Mapper;
import org.apache.ibatis.annotations.Select;

import xiaozhi.modules.binding.entity.ProfileApiKeyEntity;

@Mapper
public interface ProfileApiKeyDao {
    @Select("""
            SELECT k.owner_id, k.secret_hash, k.scopes FROM ai_profile_api_key k
            JOIN sys_user u ON u.id=k.owner_id AND u.status=1
            WHERE k.key_id=#{keyId} AND k.revoked_at IS NULL
              AND (k.expires_at IS NULL OR k.expires_at>CURRENT_TIMESTAMP(3))
            """)
    ProfileApiKeyEntity findActive(String keyId);
}
