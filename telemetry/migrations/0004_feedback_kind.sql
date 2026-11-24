ALTER TABLE feedback ADD COLUMN kind TEXT NOT NULL DEFAULT 'comment';
ALTER TABLE feedback ADD COLUMN log_head TEXT;
CREATE INDEX feedback_kind ON feedback (kind, received_at);
