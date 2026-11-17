package xiaozhi.modules.agent.dao;

import java.util.List;

import org.apache.ibatis.annotations.Mapper;
import org.apache.ibatis.annotations.Param;
import org.apache.ibatis.annotations.Insert;
import org.apache.ibatis.annotations.Select;
import org.apache.ibatis.annotations.Update;

import com.baomidou.mybatisplus.core.mapper.BaseMapper;

import xiaozhi.modules.agent.entity.AgentChatHistoryEntity;

/**
 * {@link AgentChatHistoryEntity} 智能体聊天历史记录Dao对象
 *
 * @author Goody
 * @version 1.0, 2025/4/30
 * @since 1.0.0
 */
@Mapper
public interface AiAgentChatHistoryDao extends BaseMapper<AgentChatHistoryEntity> {
    @Select("SELECT * FROM ai_agent_chat_history WHERE message_id=#{messageId} FOR UPDATE")
    AgentChatHistoryEntity lockMessage(String messageId);

    @Insert("""
            INSERT INTO ai_agent_chat_history
            (mac_address,agent_id,user_guid,session_id,chat_type,content,created_at,message_id,
             usage_session_id,boot_id,role_id,turn_id,delivery_status,owner_id,agent_name,profile_snapshot,profile_version,
             binding_version,text_truncated)
            VALUES (#{macAddress},#{agentId},#{userGuid},#{sessionId},#{chatType},#{content},#{createdAt},#{messageId},
             #{usageSessionId},#{bootId},#{roleId},#{turnId},#{deliveryStatus},#{ownerId},#{agentName},#{profileSnapshot},#{profileVersion},
             #{bindingVersion},#{textTruncated})
            ON DUPLICATE KEY UPDATE id=id
            """)
    void insertIdempotent(AgentChatHistoryEntity entity);

    @Update("UPDATE ai_agent_chat_history SET delivery_status=#{status},updated_at=CURRENT_TIMESTAMP(3) WHERE id=#{id}")
    void updateDelivery(@Param("id") Long id, @Param("status") String status);

    @Update("UPDATE ai_agent_chat_history SET audio_id=#{audioId} WHERE id=#{id} AND audio_id IS NULL")
    void attachAudio(@Param("id") Long id, @Param("audioId") String audioId);

    /**
     * 根据智能体ID删除聊天历史记录
     *
     * @param agentId 智能体ID
     */
    void deleteHistoryByAgentId(String agentId);

    /**
     * 根据智能体ID删除音频ID
     *
     * @param agentId 智能体ID
     */
    void deleteAudioIdByAgentId(String agentId);

    /**
     * 根据智能体ID获取所有音频ID列表
     *
     * @param agentId 智能体ID
     * @return 音频ID列表
     */
    List<String> getAudioIdsByAgentId(String agentId);

    /**
     * 批量删除音频
     *
     * @param audioIds 音频ID列表
     */
    void deleteAudioByIds(@Param("audioIds") List<String> audioIds);
}
