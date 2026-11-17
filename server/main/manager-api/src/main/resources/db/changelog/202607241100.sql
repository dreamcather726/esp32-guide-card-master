ALTER TABLE ai_agent_chat_history
    ADD COLUMN user_guid VARCHAR(128) NULL COMMENT 'Customer userGUID' AFTER agent_id,
    ADD INDEX idx_ai_agent_chat_history_agent_user (agent_id, user_guid);

CREATE TABLE IF NOT EXISTS ai_agent_user_memory (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    agent_id VARCHAR(32) NOT NULL COMMENT 'Agent ID',
    user_guid VARCHAR(128) NOT NULL COMMENT 'Customer userGUID',
    summary_memory TEXT NULL COMMENT 'Memory summary for this agent/user',
    created_at DATETIME(3) DEFAULT CURRENT_TIMESTAMP(3) NOT NULL,
    updated_at DATETIME(3) DEFAULT CURRENT_TIMESTAMP(3) NOT NULL
        ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uk_ai_agent_user_memory (agent_id, user_guid),
    KEY idx_ai_agent_user_memory_agent (agent_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Per-user memory for an agent';
