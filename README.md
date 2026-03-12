# pg_synclib_hash

Postgres C extension that computes row hashes at write time via a BEFORE trigger. Uses the same `synclib_hash` C library as iOS/Android/Web clients, guaranteeing cross-platform hash consistency for Merkle tree sync verification.

## How it works

1. A `BEFORE INSERT OR UPDATE` trigger fires on each synced table
2. The trigger iterates the row's columns, skipping `document` (JSONB), arrays, bytea, and `row_hash` itself
3. Booleans are normalized to `0`/`1` (matching SQLite storage)
4. Calls `synclib_build_sorted_json()` + `synclib_row_hash()` from the shared C library
5. Stores the result in a `row_hash TEXT` column on the row

The Elixir server reads precomputed hashes with `SELECT row_hash FROM table WHERE deleted_at IS NULL ORDER BY id` instead of loading full rows and computing hashes via WASM at read time.

## Deployment

### 1. Build the custom Postgres image

From the sync project root:

```bash
docker build -f pg_synclib_hash/Dockerfile -t postgres-synclib:15-alpine .
```

### 2. Update docker-compose.yml

Change the db service image:

```yaml
db:
  image: postgres-synclib:15-alpine
```

### 3. Restart and install

```bash
docker-compose up -d db
docker exec -i <db-container> psql -U postgres -d sync_server_prod -f /dev/stdin < pg_synclib_hash/install.sql
```

The install script auto-discovers synced tables (any table with both `id` and `deleted_at` columns), adds `row_hash` columns, creates triggers, and backfills existing rows.

### Quick install into a running container

```bash
./pg_synclib_hash/build.sh install
```

This copies source files into the container, compiles, installs the extension, and runs install.sql in one step.

## Re-running

`install.sql` is idempotent. Run it again after adding new tables to pick them up automatically.

## Building

### Prerequisites

- PostgreSQL development headers (`pg_config` must be in `PATH`)
- C compiler (gcc or clang)
- `synclib_hash` repo as a sibling directory (for shared C source files)

### Native (no Docker)

```bash
./build.sh              # Compile the extension
./build.sh install      # Compile + sudo install the .so into Postgres
./build.sh all [dbname] # Compile + install + run install.sql (default db: trblgd2)
```

Or using `make` directly:

```bash
make                    # Build
sudo make install       # Install into Postgres extension directory
make clean              # Clean build artifacts
```

### Docker

```bash
# Build a custom Postgres image with the extension baked in
docker build -f Dockerfile -t postgres-synclib:15-alpine ..

# Or install into a running container
./build-with-docker.sh install [container-name]
```