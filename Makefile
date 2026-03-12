# pg_synclib_hash - Postgres extension for row hash computation
#
# Builds a shared library (.so) that Postgres loads for the trigger function.
# Links synclib_hash source files directly (no separate library needed).
#
# Usage:
#   make                  # build
#   make install          # install to PG directories
#   make clean            # clean build artifacts
#
# Requires: postgresql-dev headers (pg_config must be in PATH)

SYNCLIB_DIR ?= ../synclib_hash

MODULE_big = pg_synclib_hash
OBJS = pg_synclib_hash.o synclib_sha256.o synclib_hash.o synclib_cJSON.o

EXTENSION = pg_synclib_hash
DATA = sql/pg_synclib_hash--1.0.sql

PG_CPPFLAGS = -I$(SYNCLIB_DIR)

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Fix macOS: Homebrew pg_config may embed a stale -isysroot path
ifeq ($(shell uname -s),Darwin)
  _REAL_SDK := $(shell xcrun --show-sdk-path 2>/dev/null)
  ifneq ($(_REAL_SDK),)
    override CPPFLAGS := $(shell echo '$(CPPFLAGS)' | sed 's|-isysroot [^ ]*|-isysroot $(_REAL_SDK)|g')
    override LDFLAGS := $(shell echo '$(LDFLAGS)' | sed 's|-isysroot [^ ]*|-isysroot $(_REAL_SDK)|g')
    override SHLIB_LINK := $(shell echo '$(SHLIB_LINK)' | sed 's|-isysroot [^ ]*|-isysroot $(_REAL_SDK)|g')
  endif
endif

# Compile synclib_hash sources from parent directory
synclib_sha256.o: $(SYNCLIB_DIR)/sha256.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PG_CPPFLAGS) -fPIC -c -o $@ $<

synclib_hash.o: $(SYNCLIB_DIR)/hash.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PG_CPPFLAGS) -fPIC -c -o $@ $<

synclib_cJSON.o: $(SYNCLIB_DIR)/cJSON.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PG_CPPFLAGS) -fPIC -c -o $@ $<

# LLVM bitcode rules (required when Postgres is built with --with-llvm)
synclib_sha256.bc: $(SYNCLIB_DIR)/sha256.c
	clang -Wno-ignored-attributes $(BITCODE_CFLAGS) $(CPPFLAGS) $(PG_CPPFLAGS) -fPIC -emit-llvm -c -o $@ $<

synclib_hash.bc: $(SYNCLIB_DIR)/hash.c
	clang -Wno-ignored-attributes $(BITCODE_CFLAGS) $(CPPFLAGS) $(PG_CPPFLAGS) -fPIC -emit-llvm -c -o $@ $<

synclib_cJSON.bc: $(SYNCLIB_DIR)/cJSON.c
	clang -Wno-ignored-attributes $(BITCODE_CFLAGS) $(CPPFLAGS) $(PG_CPPFLAGS) -fPIC -emit-llvm -c -o $@ $<
