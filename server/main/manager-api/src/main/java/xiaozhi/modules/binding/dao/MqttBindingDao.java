package xiaozhi.modules.binding.dao;

import java.util.Date;

import org.apache.ibatis.annotations.Insert;
import org.apache.ibatis.annotations.Mapper;
import org.apache.ibatis.annotations.Param;
import org.apache.ibatis.annotations.Select;
import org.apache.ibatis.annotations.Update;

import xiaozhi.modules.binding.entity.MqttBindingMapping;
import xiaozhi.modules.binding.entity.MqttBindingOutbox;

@Mapper
public interface MqttBindingDao {
    // Lookup only; the service then locks the device before locking/revalidating this mapping.
    @Select("SELECT * FROM ai_mqtt_binding_mapping WHERE cab_guid=#{cab}")
    MqttBindingMapping byCab(String cab);

    @Select("SELECT * FROM ai_mqtt_binding_mapping WHERE mac_address=#{mac} FOR UPDATE")
    MqttBindingMapping lockMapping(String mac);

    @Insert("""
            INSERT INTO ai_mqtt_binding_mapping (mac_address,cab_guid,mapping_id,device_id,owner_id)
            VALUES (#{macAddress},#{cabGuid},#{mappingId},#{deviceId},#{ownerId})
            """)
    void insertMapping(MqttBindingMapping mapping);

    @Update("""
            UPDATE ai_mqtt_binding_mapping SET cab_guid=#{cabGuid},mapping_id=#{mappingId},
            device_id=#{deviceId},owner_id=#{ownerId},boot_id=NULL,report_json=NULL,received_at=NULL
            WHERE mac_address=#{macAddress}
            """)
    void updateMapping(MqttBindingMapping mapping);

    @Update("UPDATE ai_mqtt_binding_mapping SET boot_id=#{bootId},report_json=#{reportJson},"
            + "received_at=#{receivedAt} WHERE mac_address=#{macAddress}")
    void observe(MqttBindingMapping mapping);

    @Select("SELECT * FROM ai_mqtt_binding_outbox WHERE mac_address=#{mac} AND cab_guid=#{cab} AND request_id=#{request}")
    MqttBindingOutbox request(@Param("mac") String mac, @Param("cab") String cab, @Param("request") String request);

    @Select("""
            SELECT * FROM ai_mqtt_binding_outbox WHERE mac_address=#{mac} AND cab_guid=#{cab}
            AND mapping_id=#{mapping} AND usage_session_id=#{session}
            AND encrypted_binding IS NOT NULL AND expires_at > #{now} ORDER BY expires_at DESC LIMIT 1
            """)
    MqttBindingOutbox reusable(@Param("mac") String mac, @Param("cab") String cab,
            @Param("mapping") String mapping, @Param("session") String session, @Param("now") Date now);

    @Insert("""
            INSERT INTO ai_mqtt_binding_outbox
            (mac_address,cab_guid,request_id,mapping_id,boot_id,user_guid,usage_session_id,binding_version,expires_at,encrypted_binding)
            VALUES (#{macAddress},#{cabGuid},#{requestId},#{mappingId},#{bootId},#{userGuid},
            #{usageSessionId},#{bindingVersion},#{expiresAt},#{encryptedBinding})
            """)
    void insertOutbox(MqttBindingOutbox outbox);

    @Update("UPDATE ai_mqtt_binding_outbox SET encrypted_binding=NULL WHERE mac_address=#{mac} AND encrypted_binding IS NOT NULL")
    void invalidateCommands(String mac);

    @Update("UPDATE ai_mqtt_binding_outbox SET encrypted_binding=NULL WHERE expires_at <= #{now} AND encrypted_binding IS NOT NULL")
    int purgeExpiredCommands(Date now);
}
