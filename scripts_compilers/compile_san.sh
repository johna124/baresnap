#!/bin/bash
set -e
CC=gcc
SAN_FLAGS="-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls "

# Detect real AES-NI for ASan
AES_FLAGS=""
if grep -q " aes " /proc/cpuinfo 2>/dev/null; then
AES_FLAGS=""
echo "🚀 [SAN] AES-NI detected on CPU"
else
echo "⚠️  [SAN] CPU without AES-NI. Skipping hardware AES flags."
fi

CFLAGS="-std=c11 -Wall -Wextra $SAN_FLAGS $AES_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t"

mkdir -p build_san

echo "== Compiling monocypher (ASan) =="
$CC $SAN_FLAGS -std=c11 -w -c third_party/monocypher/monocypher.c -o build_san/monocypher.o

echo "== Compiling aes_gcm (ASan + AES-NI) =="
$CC $SAN_FLAGS $AES_FLAGS -std=c11 -w -Ithird_party/aes-gcm -c third_party/aes-gcm/aes_gcm.c -o build_san/aes_gcm.o

echo "== Compiling xdelta3 (ASan) =="
$CC $SAN_FLAGS -std=gnu11 -w -fPIC \
-DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 \
-DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 \
-DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 \
-DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 \
-DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t \
-Ithird_party/xdelta/xdelta3 \
-c third_party/xdelta/xdelta3/xdelta3.c -o build_san/xdelta3.o

echo "== Compiling zstd (ASan) =="

cd third_party/zstd
make -C lib clean >/dev/null 2>&1 || true
make -C lib CC="$CC" CFLAGS="-g -O0 -fPIC" libzstd.a -j"$(nproc)" >/dev/null 2>&1
cd ../..

echo "== Linking ASan binary =="

NCURSES_LIBS=$(pkg-config --libs ncurses 2>/dev/null || echo "-lncurses -ltinfo")
BASE="src/brs_index.c src/brs_repo_create.c src/brs_util.c src/brs_buffer.c \
src/brs_init.c src/brs_repo_health.c src/brs_vfs_context.c src/brs_cache.c \
src/brs_lock.c src/brs_repo_ls.c src/brs_vfs_dispatch.c src/brs_chunker.c \
src/brs_lz4.c src/brs_repo_prune.c src/brs_vfs_local.c src/brs_config.c \
src/brs_manifest.c src/brs_repo_query.c src/brs_vfs_ssh.c src/brs_crypto.c \
src/brs_pack.c src/brs_repo_restore.c src/brs_zstd.c src/brs_delta.c \
src/brs_pool.c src/brs_repo_verify.c src/main.c src/brs_dir.c \
src/brs_remote.c src/brs_spsc.c src/brs_fsutil.c src/brs_tui.c \
src/brs_hash.c src/brs_repo_common.c src/brs_uri.c"

$CC $CFLAGS \
$BASE \
build_san/monocypher.o build_san/aes_gcm.o build_san/xdelta3.o \
third_party/zstd/lib/libzstd.a \
$NCURSES_LIBS -lpthread -lm \
-o build_san/baresnap

echo "== Compiling baresnap-remote (ASan) =="

$CC $CFLAGS src/baresnap-remote.c src/brs_remote.c \
-o build_san/baresnap-remote

echo ""
echo "✅ build_san/baresnap ready (ASan + UBSan + AES-NI)"
echo "   Run: ./stress_san.sh"
