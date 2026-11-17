CREATE TABLE ai_profile_api_key (
    key_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    owner_id BIGINT NOT NULL,
    secret_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    label VARCHAR(128) NOT NULL,
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    expires_at DATETIME(3) NULL,
    revoked_at DATETIME(3) NULL,
    PRIMARY KEY (key_id),
    KEY idx_profile_key_owner (owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
