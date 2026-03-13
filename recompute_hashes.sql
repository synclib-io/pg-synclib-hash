-- Recompute all row_hash values after rebuilding pg_synclib_hash.
--
-- Finds every table with a row_hash column and runs a no-op UPDATE
-- to re-trigger the BEFORE UPDATE trigger, which rewrites row_hash.
--
-- Usage:
--   psql -U postgres -d <dbname> -f recompute_hashes.sql

DO $$
DECLARE
    tbl TEXT;
    row_count BIGINT;
BEGIN
    FOR tbl IN
        SELECT table_name
        FROM information_schema.columns
        WHERE column_name = 'row_hash'
          AND table_schema = 'public'
        ORDER BY table_name
    LOOP
        RAISE NOTICE 'Recomputing row_hash for %...', tbl;
        -- The UPDATE fires the BEFORE UPDATE trigger, which unconditionally
        -- recomputes row_hash before the row is written. The NULL never lands.
        EXECUTE format('UPDATE %I SET row_hash = NULL', tbl);
        GET DIAGNOSTICS row_count = ROW_COUNT;
        RAISE NOTICE '  % rows updated', row_count;
    END LOOP;
END $$;
