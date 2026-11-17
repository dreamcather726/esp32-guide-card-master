ALTER TABLE ai_user_profile
    ADD COLUMN relationship VARCHAR(64) NULL,
    ADD COLUMN usage_count INT NOT NULL DEFAULT 0;
