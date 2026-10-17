#!/bin/bash
set -e
CC=gcc

# === TSAN REALIGNMENT MATRIX ===
# 1. Changed -fsanitize=address to -fsanitize=thread
# 2. Upgraded optimization from -O0 to -O2 (TSan mandatory structural requirement)
SAN_FLAGS="-g -O2 -fsanitize=thread,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls"

# Detect real AES-NI for the build environment
AES_FLAGS=""
if grep -q " aes " /proc/cpuinfo 2>/dev/null; then
    AES_FLAGS=""
    echo "🚀 [TSAN] AES-NI detected on CPU"
else
    echo "⚠️  [TSAN] CPU without AES-NI. Omitting hardware AES flags."
fi

# Detect system ZSTD via pkg-config
if pkg-config --exists libzstd; then
    ZSTD_CFLAGS=$(pkg-config --cflags libzstd)
    ZSTD_LIBS=$(pkg-config --libs libzstd)
    echo "📦 [SYS] Zstandard detected in the system"
else
    echo "❌ [ERROR] libzstd-dev was not found in the system. Please install it before proceeding."
    exit 1
fi

CFLAGS="-std=c11 -Wall -Wextra $SAN_FLAGS $AES_FLAGS $ZSTD_CFLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DREGRESSION_TEST=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t"

# CRITICAL FIX: Eradicate previous matrices and cleanly generate the correct target tree
rm -rf build_tsan
mkdir -p build_tsan

echo "== Compiling monocypher (TSan) =="
$CC $SAN_FLAGS -std=c11 -w -c third_party/monocypher/monocypher.c -o build_tsan/monocypher.o

echo "== Compiling aes_gcm (TSan + AES-NI) =="
$CC $SAN_FLAGS $AES_FLAGS -std=c11 -w -Ithird_party/aes-gcm -c third_party/aes-gcm/aes_gcm.c -o build_tsan/aes_gcm.o

echo "== Compiling xdelta3 (TSan) =="
$CC $SAN_FLAGS -std=gnu11 -w -fPIC \
    -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 \
    -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 \
    -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 \
    -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 \
    -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t \
    -Ithird_party/xdelta/xdelta3 \
    -c third_party/xdelta/xdelta3/xdelta3.c -o build_tsan/xdelta3.o

echo "== Linking TSan binary =="
NCURSES_LIBS=$(pkg-config --libs ncurses 2>/dev/null || echo "-lncurses -ltinfo")

SOURCES="src/brs_index.c src/brs_repo_create.c src/brs_util.c src/brs_buffer.c \
src/brs_init.c src/brs_repo_health.c src/brs_vfs_context.c src/brs_cache.c \
src/brs_lock.c src/brs_repo_ls.c src/brs_vfs_dispatch.c src/brs_chunker.c \
src/brs_lz4.c src/brs_repo_prune.c src/brs_vfs_local.c src/brs_config.c \
src/brs_manifest.c src/brs_repo_query.c src/brs_vfs_ssh.c src/brs_crypto.c \
src/brs_pack.c src/brs_repo_restore.c src/brs_zstd.c src/brs_delta.c \
src/brs_pool.c src/brs_repo_verify.c src/main.c src/brs_dir.c \
src/brs_remote.c src/brs_spsc.c src/brs_fsutil.c src/brs_tui.c \
src/brs_hash.c src/brs_repo_common.c src/brs_uri.c"

# FIXED LINKER PATTERNS: Sourcing directly from the freshly synchronized build_tsan tree
$CC $CFLAGS \
    $SOURCES \
    build_tsan/monocypher.o build_tsan/aes_gcm.o build_tsan/xdelta3.o \
    $ZSTD_LIBS \
    $NCURSES_LIBS -lpthread -lm \
    -o build_tsan/baresnap

echo "== Compiling baresnap-remote (Clean for SSH) =="
$CC -std=c11 -O2 -Wall -Wextra -Isrc -Ithird_party/aes-gcm \
    src/baresnap-remote.c \
    src/brs_remote.c \
    third_party/monocypher/monocypher.c \
    third_party/aes-gcm/aes_gcm.c \
    $ZSTD_LIBS -lpthread -lm \
    -o build_tsan/baresnap-remote

echo ""
echo "✅ build_tsan/baresnap ready using system ZSTD under TSAN supervision"
echo ""

