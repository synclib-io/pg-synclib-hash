#!/bin/bash
# Build and install pg_synclib_hash extension (native Postgres)
#
# Usage:
#   ./build.sh                       Build the extension
#   ./build.sh install               Build and install into Postgres
#   ./build.sh all [dbname]          Build, install, and run install.sql

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SYNCLIB_DIR="$PROJECT_DIR/synclib_hash"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

if ! command -v pg_config &> /dev/null; then
    echo -e "${RED}Error: pg_config not found. Install postgresql-dev.${NC}"
    exit 1
fi

case "${1:-build}" in
    build)
        echo -e "${BLUE}Building pg_synclib_hash...${NC}"
        cd "$SCRIPT_DIR"
        make SYNCLIB_DIR="$SYNCLIB_DIR"
        echo -e "${GREEN}Done. Install with: sudo make install SYNCLIB_DIR=$SYNCLIB_DIR${NC}"
        ;;
    install)
        echo -e "${BLUE}Building and installing pg_synclib_hash...${NC}"
        cd "$SCRIPT_DIR"
        make SYNCLIB_DIR="$SYNCLIB_DIR"
        sudo make install SYNCLIB_DIR="$SYNCLIB_DIR"
        echo -e "${GREEN}Done. Run install.sql to create triggers:${NC}"
        echo "  psql -U postgres -d <dbname> -f $SCRIPT_DIR/install.sql"
        ;;
    all)
        DB_NAME="${2:-trblgd2}"
        echo -e "${BLUE}Building, installing, and configuring pg_synclib_hash (db: $DB_NAME)...${NC}"
        cd "$SCRIPT_DIR"
        make SYNCLIB_DIR="$SYNCLIB_DIR"
        sudo make install SYNCLIB_DIR="$SYNCLIB_DIR"
        psql -U postgres -d "$DB_NAME" -f "$SCRIPT_DIR/install.sql"
        echo -e "${GREEN}pg_synclib_hash fully installed and configured.${NC}"
        ;;
    *)
        echo "Usage: $0 {build|install|all [dbname]}"
        exit 1
        ;;
esac
