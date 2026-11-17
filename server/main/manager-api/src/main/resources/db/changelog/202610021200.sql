CREATE TABLE ai_user_profile (
    owner_id BIGINT NOT NULL,
    user_guid VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    nickname VARCHAR(64) NULL,
    language VARCHAR(32) NULL,
    interests VARCHAR(1024) NULL,
    explanation_style VARCHAR(512) NULL,
    profile_version BIGINT NOT NULL DEFAULT 1,
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (owner_id, user_guid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- This row survives device removal so a MAC's version is never reused.
CREATE TABLE ai_device_binding_state (
    mac_address VARCHAR(50) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
    binding_version BIGINT NOT NULL DEFAULT 0,
    active_session_id VARCHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE ai_usage_session (
    id VARCHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
    mac_address VARCHAR(50) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    device_id VARCHAR(32) NOT NULL,
    owner_id BIGINT NOT NULL,
    user_guid VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    boot_id VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    binding_version BIGINT NOT NULL,
    agent_id VARCHAR(32) NOT NULL,
    agent_name VARCHAR(255) NULL,
    profile_snapshot TEXT NOT NULL,
    profile_version BIGINT NOT NULL,
    token_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    expires_at DATETIME(3) NOT NULL,
    revoked_at DATETIME(3) NULL,
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UNIQUE KEY uk_usage_device_version (mac_address, binding_version),
    KEY idx_usage_owner_user (owner_id, user_guid, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

ALTER TABLE ai_agent_chat_history
    ADD COLUMN message_id VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NULL,
    ADD COLUMN usage_session_id VARCHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL,
    ADD COLUMN boot_id VARCHAR(128) NULL,
    ADD COLUMN role_id VARCHAR(128) NULL,
    ADD COLUMN turn_id VARCHAR(128) NULL,
    ADD COLUMN delivery_status VARCHAR(32) NULL,
    ADD COLUMN binding_version BIGINT NULL,
    ADD COLUMN text_truncated TINYINT(1) NULL,
    ADD COLUMN owner_id BIGINT NULL,
    ADD COLUMN agent_name VARCHAR(255) NULL,
    ADD COLUMN profile_snapshot TEXT NULL,
    ADD COLUMN profile_version BIGINT NULL,
    ADD UNIQUE KEY uk_chat_message_id (message_id),
    ADD KEY idx_chat_usage_session (usage_session_id, created_at, id),
    ADD KEY idx_chat_owner_user (owner_id, user_guid);
