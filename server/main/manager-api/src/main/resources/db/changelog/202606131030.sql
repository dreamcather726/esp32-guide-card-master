CREATE TABLE IF NOT EXISTS ai_agent_role_voice_binding (
    id VARCHAR(64) NOT NULL COMMENT 'Binding ID',
    agent_id VARCHAR(64) NOT NULL COMMENT 'Agent ID',
    name VARCHAR(64) NULL COMMENT 'Binding name',
    system_prompt TEXT NULL COMMENT 'System prompt',
    tts_voice_id VARCHAR(64) NULL COMMENT 'TTS voice ID',
    sort INT DEFAULT 0 COMMENT 'Sort order',
    enabled TINYINT(1) DEFAULT 1 COMMENT 'Enabled flag',
    creator BIGINT NULL COMMENT 'Creator',
    created_at DATETIME NULL COMMENT 'Created time',
    updater BIGINT NULL COMMENT 'Updater',
    updated_at DATETIME NULL COMMENT 'Updated time',
    PRIMARY KEY (id),
    KEY idx_agent_role_voice_binding_agent_sort (agent_id, sort)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Agent role and voice bindings';
