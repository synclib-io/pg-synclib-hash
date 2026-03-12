-- pg_synclib_hash extension
-- Provides a trigger function that computes row hashes for Merkle tree sync verification

-- The trigger function: computes SHA256(id + "|" + sorted_json(columns)) and stores in row_hash
CREATE FUNCTION synclib_compute_row_hash()
RETURNS trigger
AS 'pg_synclib_hash', 'synclib_compute_row_hash_trigger'
LANGUAGE C;
