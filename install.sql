-- pg_synclib_hash installation (bootstrap only)
--
-- This script only installs the Postgres extension (shared library).
-- It does NOT create triggers or add columns.
--
-- To set up triggers with the correct hash_columns config, use:
--   mix synclib.setup_hash_triggers
--
-- The mix task reads hash_columns from the app config (tg.exs / jumpcut.exs),
-- which is the single source of truth shared with clients.
--
-- Usage:
--   psql -U postgres -d sync_server_prod -f install.sql

CREATE EXTENSION IF NOT EXISTS pg_synclib_hash;

SELECT 'pg_synclib_hash extension installed. Run: mix synclib.setup_hash_triggers' AS status;
