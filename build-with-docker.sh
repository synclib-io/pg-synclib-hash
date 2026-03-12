#!/bin/bash
# Build pg_synclib_hash extension
#
# Usage:
#   ./build.sh docker     Build Docker image with extension (recommended)
#   ./build.sh native     Build natively (requires pg_config in PATH)
#   ./build.sh install    Build and install into running Docker container

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

build_docker() {
    echo -e "${BLUE}Building custom Postgres image with pg_synclib_hash...${NC}"

    docker build \
        -f "$SCRIPT_DIR/Dockerfile" \
        -t postgres-synclib:15-alpine \
        "$PROJECT_DIR"

    echo -e "${GREEN}✓ Built: postgres-synclib:15-alpine${NC}"
    echo ""
    echo "Update docker-compose.yml db service:"
    echo "  image: postgres-synclib:15-alpine"
    echo ""
    echo "Then run the install SQL:"
    echo "  docker exec -i <container> psql -U postgres -d sync_server_prod < pg_synclib_hash/install.sql"
}

build_native() {
    echo -e "${BLUE}Building pg_synclib_hash natively...${NC}"

    if ! command -v pg_config &> /dev/null; then
        echo -e "${RED}Error: pg_config not found. Install postgresql-dev.${NC}"
        exit 1
    fi

    cd "$SCRIPT_DIR"
    make SYNCLIB_DIR="$PROJECT_DIR/synclib_hash"

    echo -e "${GREEN}✓ Built: pg_synclib_hash.so${NC}"
    echo ""
    echo "Install with: make install SYNCLIB_DIR=$PROJECT_DIR/synclib_hash"
}

install_to_container() {
    echo -e "${BLUE}Building and installing into running Docker container...${NC}"

    # Find the postgres container
    CONTAINER=$(docker ps --filter "ancestor=postgres:15-alpine" --format "{{.Names}}" | head -1)
    if [ -z "$CONTAINER" ]; then
        CONTAINER=$(docker ps --filter "ancestor=postgres-synclib:15-alpine" --format "{{.Names}}" | head -1)
    fi
    if [ -z "$CONTAINER" ]; then
        echo -e "${RED}Error: No running postgres container found.${NC}"
        echo "Pass container name as argument: $0 install <container_name>"
        CONTAINER="${2:-}"
        if [ -z "$CONTAINER" ]; then
            exit 1
        fi
    fi

    echo "Using container: $CONTAINER"

    # Install build tools in container
    docker exec "$CONTAINER" apk add --no-cache build-base clang

    # Copy source files
    docker exec "$CONTAINER" mkdir -p /build/synclib_hash /build/pg_synclib_hash/sql
    for f in sha256.h sha256.c hash.h hash.c cJSON.h cJSON.c; do
        docker cp "$PROJECT_DIR/synclib_hash/$f" "$CONTAINER:/build/synclib_hash/"
    done
    docker cp "$SCRIPT_DIR/pg_synclib_hash.c" "$CONTAINER:/build/pg_synclib_hash/"
    docker cp "$SCRIPT_DIR/Makefile" "$CONTAINER:/build/pg_synclib_hash/"
    docker cp "$SCRIPT_DIR/pg_synclib_hash.control" "$CONTAINER:/build/pg_synclib_hash/"
    docker cp "$SCRIPT_DIR/sql/pg_synclib_hash--1.0.sql" "$CONTAINER:/build/pg_synclib_hash/sql/"

    # Build and install
    docker exec -w /build/pg_synclib_hash "$CONTAINER" \
        make SYNCLIB_DIR=/build/synclib_hash
    docker exec -w /build/pg_synclib_hash "$CONTAINER" \
        make install SYNCLIB_DIR=/build/synclib_hash

    # Run install SQL
    docker cp "$SCRIPT_DIR/install.sql" "$CONTAINER:/build/"
    docker exec "$CONTAINER" psql -U postgres -d sync_server_prod -f /build/install.sql

    # Clean up build files
    docker exec "$CONTAINER" rm -rf /build

    echo -e "${GREEN}✓ pg_synclib_hash installed and configured!${NC}"
}

case "${1:-docker}" in
    docker)
        build_docker
        ;;
    native)
        build_native
        ;;
    install)
        install_to_container "$@"
        ;;
    *)
        echo "Usage: $0 {docker|native|install [container_name]}"
        exit 1
        ;;
esac
