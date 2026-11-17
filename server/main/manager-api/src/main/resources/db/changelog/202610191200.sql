ALTER TABLE ai_agent_role_voice_binding
    ADD COLUMN image_url VARCHAR(2048) NULL,
    ADD COLUMN video_url VARCHAR(2048) NULL,
    ADD COLUMN audio_url VARCHAR(2048) NULL;

ALTER TABLE ai_profile_api_key
    ADD COLUMN scopes VARCHAR(512) NOT NULL DEFAULT 'profile';

CREATE TABLE ai_device_role_binding (
    device_id VARCHAR(32) NOT NULL PRIMARY KEY,
    owner_id BIGINT NOT NULL,
    agent_id VARCHAR(32) NOT NULL,
    role_id VARCHAR(64) NOT NULL,
    binding_version BIGINT NOT NULL DEFAULT 1,
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);

CREATE TABLE ai_device_history_clear (
    owner_id BIGINT NOT NULL,
    mac_address VARCHAR(50) NOT NULL,
    cleared_at DATETIME(3) NOT NULL,
    PRIMARY KEY (owner_id, mac_address)
);

CREATE TABLE ai_chat_history_tombstone (
    message_id VARCHAR(128) NOT NULL PRIMARY KEY,
    deleted_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
);
