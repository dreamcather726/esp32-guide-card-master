package xiaozhi.modules.binding.dao;

import java.util.List;

import org.apache.ibatis.annotations.Insert;
import org.apache.ibatis.annotations.Mapper;
import org.apache.ibatis.annotations.Param;
import org.apache.ibatis.annotations.Select;
import org.apache.ibatis.annotations.Update;

import xiaozhi.modules.binding.entity.DeviceBindingState;
import xiaozhi.modules.binding.entity.UsageSessionEntity;
import xiaozhi.modules.binding.entity.UserProfileEntity;
import xiaozhi.modules.device.entity.DeviceEntity;
import xiaozhi.modules.agent.entity.AgentChatHistoryEntity;

@Mapper
public interface UserBindingDao {
    @Select("SELECT * FROM ai_device WHERE mac_address = #{mac} FOR UPDATE")
    DeviceEntity lockDevice(String mac);

    @Insert("INSERT INTO ai_device_binding_state (mac_address, binding_version) VALUES (#{mac}, 0) "
            + "ON DUPLICATE KEY UPDATE mac_address = VALUES(mac_address)")
    void ensureState(String mac);

    @Select("SELECT * FROM ai_device_binding_state WHERE mac_address = #{mac} FOR UPDATE")
    DeviceBindingState lockState(String mac);

    @Update("UPDATE ai_device_binding_state SET binding_version=#{bindingVersion}, "
            + "active_session_id=#{activeSessionId} WHERE mac_address=#{macAddress}")
    void updateState(DeviceBindingState state);

    @Select("SELECT * FROM ai_usage_session WHERE id=#{id}")
    UsageSessionEntity session(String id);

    @Select("SELECT * FROM ai_usage_session WHERE id=#{id} FOR UPDATE")
    UsageSessionEntity lockSession(String id);

    @Select("SELECT * FROM ai_agent_chat_history WHERE owner_id=#{owner} AND user_guid=#{guid} "
            + "AND usage_session_id=#{session} ORDER BY created_at ASC,id ASC LIMIT #{limit} OFFSET #{offset}")
    List<AgentChatHistoryEntity> messages(@Param("owner") Long owner, @Param("guid") String guid,
            @Param("session") String session, @Param("limit") int limit, @Param("offset") int offset);

    @Insert("""
            INSERT INTO ai_usage_session
            (id, mac_address, device_id, owner_id, user_guid, boot_id, binding_version,
             agent_id, agent_name, profile_snapshot, profile_version, token_hash, expires_at, created_at)
            VALUES (#{id}, #{macAddress}, #{deviceId}, #{ownerId}, #{userGuid}, #{bootId}, #{bindingVersion},
             #{agentId}, #{agentName}, #{profileSnapshot}, #{profileVersion}, #{tokenHash}, #{expiresAt}, #{createdAt})
            """)
    void insertSession(UsageSessionEntity session);

    @Update("UPDATE ai_usage_session SET revoked_at=CURRENT_TIMESTAMP(3) WHERE id=#{id} AND revoked_at IS NULL")
    void revokeSession(String id);

    @Select("SELECT * FROM ai_user_profile WHERE owner_id=#{owner} AND user_guid=#{guid}")
    UserProfileEntity profile(@Param("owner") Long owner, @Param("guid") String guid);

    @Select("SELECT * FROM ai_user_profile WHERE owner_id=#{owner} AND user_guid LIKE #{pattern} ESCAPE '!' "
            + "ORDER BY (user_guid=#{prefix}) DESC, user_guid ASC LIMIT 30")
    List<UserProfileEntity> profilesByPrefix(@Param("owner") Long owner, @Param("prefix") String prefix,
            @Param("pattern") String pattern);

    @Insert("""
            INSERT INTO ai_user_profile (owner_id, user_guid, nickname, language, interests, explanation_style,
                relationship, usage_count, profile_version)
            VALUES (#{ownerId}, #{userGuid}, #{nickname}, #{language}, #{interests}, #{explanationStyle},
                #{relationship}, COALESCE(#{usageCount}, 0), 1)
            ON DUPLICATE KEY UPDATE nickname=VALUES(nickname), language=VALUES(language),
            interests=VALUES(interests), explanation_style=VALUES(explanation_style),
            relationship=COALESCE(#{relationship}, relationship), usage_count=COALESCE(#{usageCount}, usage_count),
            profile_version=profile_version+1
            """)
    void upsertProfile(UserProfileEntity profile);

    @Insert("INSERT INTO ai_user_profile (owner_id,user_guid,profile_version) VALUES (#{owner},#{guid},1) "
            + "ON DUPLICATE KEY UPDATE owner_id=VALUES(owner_id)")
    void ensureProfile(@Param("owner") Long owner, @Param("guid") String guid);

    @Select("SELECT * FROM ai_usage_session WHERE owner_id=#{owner} AND user_guid=#{guid} "
            + "ORDER BY created_at DESC,id DESC LIMIT #{limit} OFFSET #{offset}")
    List<UsageSessionEntity> sessions(@Param("owner") Long owner, @Param("guid") String guid,
            @Param("limit") int limit, @Param("offset") int offset);
}
