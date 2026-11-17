CREATE TABLE ai_mqtt_binding_mapping (
    mac_address VARCHAR(50) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
    cab_guid VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    mapping_id VARCHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    device_id VARCHAR(32) NOT NULL,
    owner_id BIGINT NOT NULL,
    boot_id VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NULL,
    report_json TEXT NULL,
    received_at DATETIME(3) NULL,
    UNIQUE KEY uk_mqtt_cab (cab_guid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Retain request tombstones; only the short-lived authenticated ciphertext is purged.
CREATE TABLE ai_mqtt_binding_outbox (
    mac_address VARCHAR(50) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    cab_guid VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    request_id VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    mapping_id VARCHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    boot_id VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    user_guid VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    usage_session_id VARCHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    binding_version BIGINT NOT NULL,
    expires_at DATETIME(3) NOT NULL,
    encrypted_binding MEDIUMTEXT NULL,
    PRIMARY KEY (mac_address,cab_guid,request_id),
    KEY idx_mqtt_outbox_session (mac_address,usage_session_id),
    KEY idx_mqtt_outbox_expiry (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
