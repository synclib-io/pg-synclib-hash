# pg_synclib_hash

Postgres C extension that computes row hashes at write time via a BEFORE trigger. This is the **single source of truth** for `row_hash` — clients store the server-computed value and use it for Merkle tree comparison without computing hashes locally.

Uses the same `synclib_hash` C library as all other platforms, guaranteeing cross-platform hash consistency.

## How it works

1. A `BEFORE INSERT OR UPDATE` trigger fires on each synced table
2. The trigger iterates the row's columns, building a JSON object from their values
3. Skips only `row_hash` (the computed column itself) and BYTEA columns (no cross-platform canonical form)
4. Booleans are normalized to `0`/`1` (matching SQLite storage)
5. Arrays (text[]) are converted to JSON strings via `array_to_json()` + `json_escape_into()` (matching SQLite TEXT storage)
6. JSONB/JSON columns are embedded as raw JSON objects (the C library recursively sorts keys)
7. Calls `synclib_build_sorted_json_from_json()` + `synclib_row_hash()` from the shared C library
8. Stores the result in a `row_hash TEXT` column on the row

The Elixir server reads precomputed hashes with `SELECT row_hash FROM table WHERE deleted_at IS NULL ORDER BY id` instead of loading full rows and computing hashes via WASM at read time.

## Building

### Prerequisites

- PostgreSQL development headers (`pg_config` must be in `PATH`)
- C compiler (gcc or clang)
- `synclib_hash` repo as a sibling directory (for shared C source files)

### Native build

```bash
make                    # Build
sudo make install       # Install .dylib/.so into Postgres extension directory
make clean              # Clean build artifacts
```

Or use the helper script:

```bash
./build.sh              # Compile the extension
./build.sh install      # Compile + sudo install into Postgres
./build.sh all [dbname] # Compile + install + run install.sql (default db: trblgd2)
```

`make install` copies files to the directories reported by `pg_config`:
- `.dylib`/`.so` → `pg_config --pkglibdir` (e.g. `/opt/homebrew/lib/postgresql@17/`)
- `.control` + SQL → `pg_config --sharedir`/extension/

### Docker build

```bash
# Build a custom Postgres image with the extension baked in
docker build -f Dockerfile -t postgres-synclib:15-alpine ..

# Or install into a running container
./build-with-docker.sh install [container-name]
```

## After rebuilding

Postgres caches loaded shared libraries in memory. After installing a new build:

1. **Restart Postgres** so it loads the updated library:
   ```bash
   # macOS (Homebrew)
   brew services restart postgresql@17

   # Linux (systemd)
   sudo systemctl restart postgresql

   # Docker
   docker restart <db-container>
   ```

2. **Recompute all row hashes** — the trigger fires on UPDATE, so a no-op update on each table will rewrite every `row_hash`:
   ```sql
   -- Find all tables with row_hash columns:
   SELECT table_name FROM information_schema.columns
   WHERE column_name = 'row_hash' AND table_schema = 'public';

   -- Then for each table:
   UPDATE users SET seqnum = seqnum;
   UPDATE conversations SET seqnum = seqnum;
   -- etc.
   ```

## Deployment (first install)

### Native Postgres

```bash
sudo make install
psql -U postgres -d <dbname> -f install.sql
mix synclib.setup_hash_triggers   # creates triggers + row_hash columns per app config
```

### Docker

1. Build the custom image:
   ```bash
   docker build -f pg_synclib_hash/Dockerfile -t postgres-synclib:15-alpine .
   ```

2. Update docker-compose.yml:
   ```yaml
   db:
     image: postgres-synclib:15-alpine
   ```

3. Restart and install:
   ```bash
   docker-compose up -d db
   docker exec -i <db-container> psql -U postgres -d sync_server_prod -f /dev/stdin < pg_synclib_hash/install.sql
   ```

### Quick install into a running container

```bash
./pg_synclib_hash/build-with-docker.sh install
```

This copies source files into the container, compiles, installs the extension, and runs install.sql in one step.

`install.sql` is idempotent — safe to re-run after adding new tables.