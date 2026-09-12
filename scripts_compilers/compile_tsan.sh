#!/bin/bash
set -e
CC=gcc

# ============================================================
# ThreadSanitizer (TSan)
# IMPORTANT: We use -O1. TSan needs minimal optimization to
# correctly instrument C11 atomics and avoid false positives.
# It cannot be mixed with ASan or UBSan.
# ============================================================

TSAN_FLAGS="-g -O1 -fsanitize=thread -fno-omit-frame-pointer"
# ============================================================
# CPU detection for AES-NI flags
# ============================================================
CPUFLAGS="$(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null || true)"
AES_FLAGS=""
if echo " $CPUFLAGS " | grep -q " aes " && \
echo " $CPUFLAGS " | grep -q " pclmulqdq " && \
echo " $CPUFLAGS " | grep -q " sse4_1 " && \
echo " $CPUFLAGS " | grep -q " ssse3 "; then
AES_FLAGS="-maes -mpclmul -msse4.1 -mssse3"
echo "🚀 CPU with AES-NI detected: enabling hardware acceleration"
else
echo "⚠️  CPU without full AES-NI: using software fallback for AES-256-GCM"
fi

CFLAGS="-std=c11 -Wall -Wextra $TSAN_FLAGS $AES_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t"

mkdir -p build_tsan
echo "== Compiling monocypher (TSan) =="
$CC $TSAN_FLAGS -std=c11 -w -c third_party/monocypher/monocypher.c -o build_tsan/monocypher.o

echo "== Compiling aes_gcm (TSan + AES-NI) =="
$CC $TSAN_FLAGS $AES_FLAGS -std=c11 -w -Ithird_party/aes-gcm -c third_party/aes-gcm/aes_gcm.c -o build_tsan/aes_gcm.o

echo "== Compiling xdelta3 (TSan) =="
$CC $TSAN_FLAGS -std=gnu11 -w -fPIC \
-DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 \
-DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 \
-DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 \
-DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 \
-DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t \
-Ithird_party/xdelta/xdelta3 \
-c third_party/xdelta/xdelta3/xdelta3.c -o build_tsan/xdelta3.o

echo "== Compiling zstd (TSan) =="
cd third_party/zstd
make -C lib clean >/dev/null 2>&1 || true
# We compile zstd with TSan so that races inside the library are also detected
make -C lib CC="$CC" CFLAGS="-g -O1 -fPIC -fsanitize=thread -fno-omit-frame-pointer" libzstd.a -j"$(nproc)" >/dev/null 2>&1
cd ../..

echo "== Linking TSan binary =="
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
build_tsan/monocypher.o build_tsan/aes_gcm.o build_tsan/xdelta3.o \
third_party/zstd/lib/libzstd.a \
$NCURSES_LIBS -lpthread -lm -fsanitize=thread \
-o build_tsan/baresnap

echo "== Compiling baresnap-remote (TSan) =="
$CC $CFLAGS src/baresnap-remote.c src/brs_remote.c -fsanitize=thread \
-o build_tsan/baresnap-remote

echo ""
echo "✅ build_tsan/baresnap ready (ThreadSanitizer)"
echo ""
echo "🚀 To run the mega_test with TSan, use:"
echo "   TSAN_OPTIONS='halt_on_error=1:history_size=7' BARESNAP=./build_tsan/baresnap ./mega_test.sh"
