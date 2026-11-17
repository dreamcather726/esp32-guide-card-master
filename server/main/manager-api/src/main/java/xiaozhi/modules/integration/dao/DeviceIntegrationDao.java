package xiaozhi.modules.integration.dao;

import java.util.Date;
import java.util.List;

import org.apache.ibatis.annotations.*;
import xiaozhi.modules.device.entity.DeviceEntity;
import xiaozhi.modules.agent.entity.AgentChatHistoryEntity;
import xiaozhi.modules.integration.dto.DeviceIntegrationDTOs.RoleBinding;

@Mapper
public interface DeviceIntegrationDao {
    String OWNED_HISTORY = "h.mac_address=#{mac} AND (h.owner_id=#{owner} OR (h.owner_id IS NULL "
            + "AND EXISTS (SELECT 1 FROM ai_agent a WHERE a.id=h.agent_id AND a.user_id=#{owner})))";

    @Select("""
            SELECT DISTINCT d.* FROM ai_device d LEFT JOIN ai_mqtt_binding_mapping m
              ON m.device_id=d.id AND m.owner_id=d.user_id AND m.mac_address=d.mac_address
            WHERE d.user_id=#{owner} AND (d.id=#{identifier} OR d.mac_address=#{mac}
              OR m.cab_guid=#{identifier} OR m.cab_guid=CONCAT(#{identifier},'s3'))
            LIMIT 2
            """)
    List<DeviceEntity> devices(@Param("owner") Long owner, @Param("identifier") String identifier,
                               @Param("mac") String mac);

    @Select("""
            SELECT b.device_id,d.mac_address,b.agent_id,b.role_id,b.binding_version,b.updated_at
            FROM ai_device_role_binding b JOIN ai_device d ON d.id=b.device_id
              AND d.user_id=b.owner_id AND d.agent_id=b.agent_id
            JOIN ai_agent_role_voice_binding r ON r.id=b.role_id AND r.agent_id=b.agent_id AND r.enabled=1
            WHERE b.device_id=#{deviceId}
            """)
    RoleBinding binding(String deviceId);

    @Insert("""
            INSERT INTO ai_device_role_binding(device_id,owner_id,agent_id,role_id)
            VALUES(#{device.id},#{device.userId},#{device.agentId},#{roleId})
            ON DUPLICATE KEY UPDATE owner_id=VALUES(owner_id),agent_id=VALUES(agent_id),
              role_id=VALUES(role_id),binding_version=binding_version+1,updated_at=CURRENT_TIMESTAMP(3)
            """)
    void saveBinding(@Param("device") DeviceEntity device, @Param("roleId") String roleId);

    @Select("SELECT COUNT(*) FROM ai_agent_chat_history h WHERE " + OWNED_HISTORY)
    long historyCount(@Param("owner") Long owner, @Param("mac") String mac);

    @Select("SELECT h.* FROM ai_agent_chat_history h WHERE " + OWNED_HISTORY
            + " ORDER BY h.created_at DESC,h.id DESC LIMIT #{limit} OFFSET #{offset}")
    List<AgentChatHistoryEntity> history(@Param("owner") Long owner, @Param("mac") String mac,
                                        @Param("limit") int limit, @Param("offset") long offset);

    @Insert("INSERT IGNORE INTO ai_chat_history_tombstone(message_id) "
            + "SELECT h.message_id FROM ai_agent_chat_history h WHERE " + OWNED_HISTORY + " AND h.message_id IS NOT NULL")
    void rememberDeletedIds(@Param("owner") Long owner, @Param("mac") String mac);

    @Delete("DELETE audio FROM ai_agent_chat_audio audio JOIN ai_agent_chat_history h ON h.audio_id=audio.id "
            + "WHERE " + OWNED_HISTORY + " AND NOT EXISTS (SELECT 1 FROM ai_agent_chat_history other "
            + "WHERE other.audio_id=audio.id AND (other.mac_address=#{mac} AND (other.owner_id=#{owner} "
            + "OR (other.owner_id IS NULL AND EXISTS (SELECT 1 FROM ai_agent a WHERE a.id=other.agent_id AND a.user_id=#{owner})))) IS NOT TRUE)")
    void deleteExclusiveAudio(@Param("owner") Long owner, @Param("mac") String mac);

    @Delete("DELETE h FROM ai_agent_chat_history h WHERE " + OWNED_HISTORY)
    int deleteHistory(@Param("owner") Long owner, @Param("mac") String mac);

    @Insert("""
            INSERT INTO ai_device_history_clear(owner_id,mac_address,cleared_at)
            VALUES(#{owner},#{mac},CURRENT_TIMESTAMP(3))
            ON DUPLICATE KEY UPDATE cleared_at=CURRENT_TIMESTAMP(3)
            """)
    void saveClear(@Param("owner") Long owner, @Param("mac") String mac);

    @Select("SELECT cleared_at FROM ai_device_history_clear WHERE owner_id=#{owner} AND mac_address=#{mac}")
    Date clearedAt(@Param("owner") Long owner, @Param("mac") String mac);

    @Select("SELECT COUNT(*) FROM ai_chat_history_tombstone WHERE message_id=#{messageId}")
    int deletedMessage(String messageId);
}
