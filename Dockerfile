# Custom Postgres image with pg_synclib_hash extension pre-installed
#
# Build from the sync project root:
#   docker build -f pg_synclib_hash/Dockerfile -t postgres-synclib:15-alpine .
#
# Then update docker-compose.yml db service:
#   image: postgres-synclib:15-alpine

FROM postgres:15-alpine AS builder

# Install build tools
RUN apk add --no-cache build-base clang

# Copy synclib_hash source files
COPY synclib_hash/sha256.h  /build/synclib_hash/
COPY synclib_hash/sha256.c  /build/synclib_hash/
COPY synclib_hash/hash.h    /build/synclib_hash/
COPY synclib_hash/hash.c    /build/synclib_hash/
COPY synclib_hash/cJSON.h   /build/synclib_hash/
COPY synclib_hash/cJSON.c   /build/synclib_hash/

# Copy extension source
COPY pg_synclib_hash/pg_synclib_hash.c      /build/pg_synclib_hash/
COPY pg_synclib_hash/Makefile               /build/pg_synclib_hash/
COPY pg_synclib_hash/pg_synclib_hash.control /build/pg_synclib_hash/
COPY pg_synclib_hash/sql/                   /build/pg_synclib_hash/sql/

WORKDIR /build/pg_synclib_hash
RUN make SYNCLIB_DIR=/build/synclib_hash && make install SYNCLIB_DIR=/build/synclib_hash

# Final image - clean postgres with just the extension installed
FROM postgres:15-alpine

# Copy built extension from builder
COPY --from=builder /usr/local/lib/postgresql/pg_synclib_hash.so /usr/local/lib/postgresql/
COPY --from=builder /usr/local/share/postgresql/extension/pg_synclib_hash* /usr/local/share/postgresql/extension/
