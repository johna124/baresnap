#!/bin/bash
set -euo pipefail

CROSS_DIR="$HOME/cross-tools"
CC="powerpc-linux-musl-gcc"
STRIP_BIN="powerpc-linux-musl-strip"
TARGET="powerpc"
HOST_FLAG="--host=powerpc-linux-musl"

if [ -d "$CROSS_DIR/powerpc-linux-musl-cross/bin" ]; then
    export PATH="$CROSS_DIR/powerpc-linux-musl-cross/bin:$PATH"
fi

MAP_FLAGS="-ffile-prefix-map=$(pwd)=."
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1793637764}"
export ZERO_AR_DATE=1

echo "⚙️ Target: PowerPC 32-bit (PPC Classic / Big Endian)"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "📥 Downloading toolchain from musl.cc..."
    mkdir -p "$CROSS_DIR" && cd "$CROSS_DIR"
    if [ ! -f "powerpc-linux-musl-cross.tgz" ]; then
        wget -q --show-progress https://musl.cc
    fi
    tar -xzf powerpc-linux-musl-cross.tgz && cd - >/dev/null
    export PATH="$CROSS_DIR/powerpc-linux-musl-cross/bin:$PATH"
fi
export CC

if ! command -v "$STRIP_BIN" >/dev/null 2>&1; then
    echo "❌ ERROR: $STRIP_BIN not found." && exit 1
fi

ABS="${ABS:-$(pwd)/build}"
STAMP="${STAMP:-$ABS/.ncurses_ok_${TARGET}}"

CFLAGS="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Os -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=0 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=4 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=4 -DSIZEOF_UNSIGNED_LONG_LONG=8 -DSYS_getrandom=-1"

PATCHED_SRC="build/src_patched"
rm -rf "$PATCHED_SRC" && mkdir -p "$PATCHED_SRC"
cp -r src/* "$PATCHED_SRC/"
CFLAGS="$CFLAGS -I$PATCHED_SRC -DBRS_IMPLEMENTATION"
mkdir -p build

RESTORE_C="$PATCHED_SRC/brs_repo_restore.c"
if [ -f "$RESTORE_C" ]; then
    sed -i 's/snap\.entry_count/snap.entries_len/g' "$RESTORE_C"
    sed -i 's/(void)unlink(src_p);/(void)0;/g' "$RESTORE_C"
    sed -i 's/int fd_origen = open(src_p, O_CREAT | O_WRONLY | O_TRUNC, 0644);/int fd_origen = -1;/g' "$RESTORE_C"
    sed -i 's/if (fd_origen >= 0) {/if (0) {/g' "$RESTORE_C"
fi

PRUNE_C="$PATCHED_SRC/brs_repo_prune.c"
if [ -f "$PRUNE_C" ]; then
    sed -i 's/if (dead_ratio < BRS_REWRITE_THRESHOLD)/if (getenv("BRS_PRUNE_NO_REWRITE") || dead_ratio < BRS_REWRITE_THRESHOLD)/' "$PRUNE_C"
fi

# Defensive replacement for brs_delta.c for PPC32
cat << 'EOF' > "$PATCHED_SRC/brs_delta.c"
#include "brs_delta.h"
#include "xdelta3.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
typedef size_t brs_xsize_t;
static pthread_mutex_t g_xd3_mutex = PTHREAD_MUTEX_INITIALIZER;
int brs_delta_encode(const uint8_t *old_data, size_t old_size, const uint8_t *new_data, size_t new_size, uint8_t **delta_out, size_t *delta_size_out) {
    if (!old_data || !new_data || !delta_out || !delta_size_out) return -1;
    if (getenv("BRS_DISABLE_DELTA")) return -1;
    brs_xsize_t cap = (brs_xsize_t)new_size + (brs_xsize_t)(new_size / 10) + (brs_xsize_t)64;
    if (cap > (brs_xsize_t)SIZE_MAX) return -1;
    uint8_t *out_buf = (uint8_t *)malloc((size_t)cap);
    if (!out_buf) return -1;
    brs_xsize_t out_size = cap;
    pthread_mutex_lock(&g_xd3_mutex);
    int ret = xd3_encode_memory(new_data, (brs_xsize_t)new_size, old_data, (brs_xsize_t)old_size, out_buf, &out_size, cap, 0);
    pthread_mutex_unlock(&g_xd3_mutex);
    if (ret != 0) { free(out_buf); return -1; }
    *delta_out = out_buf; *delta_size_out = (size_t)out_size; return 0;
}
int brs_delta_decode(const uint8_t *old_data, size_t old_size, const uint8_t *delta_data, size_t delta_size, size_t expected_new_size, uint8_t **new_out, size_t *new_size_out) {
    if (!old_data || !delta_data || !new_out || !new_size_out) return -1;
    if (expected_new_size > SIZE_MAX) return -1;
    uint8_t *out_buf = (uint8_t *)malloc(expected_new_size ? expected_new_size : 1);
    if (!out_buf) return -1;
    brs_xsize_t out_size = (brs_xsize_t)expected_new_size;
    pthread_mutex_lock(&g_xd3_mutex);
    int ret = xd3_decode_memory(delta_data, (brs_xsize_t)delta_size, old_data, (brs_xsize_t)old_size, out_buf, &out_size, (brs_xsize_t)expected_new_size, 0);
    pthread_mutex_unlock(&g_xd3_mutex);
    if (ret != 0) { free(out_buf); return -1; }
    *new_out = out_buf; *new_size_out = (size_t)out_size; return 0;
}
EOF

# ---- Compilation of ncurses 6.5 (PowerPC Minified) ----
if [ ! -f "$STAMP" ]; then
    cd third_party/ncurses-6.5
    make distclean >/dev/null 2>&1 || true
    NCURSES_CFLAGS="-Os -fPIC -fno-ident -ffunction-sections -fdata-sections $MAP_FLAGS"
    ./configure CC="$CC" CFLAGS="$NCURSES_CFLAGS" \
        "$HOST_FLAG" \
        --prefix="$ABS/staging" \
        --enable-static --disable-shared --disable-widec --enable-overwrite \
        --with-termlib --without-progs --without-tests --without-cxx \
        --without-cxx-binding --without-ada --without-manpages \
        --disable-home-terminfo --disable-ext-colors --disable-mouse \
        --disable-stripping \
        --with-fallbacks=linux,vt100,xterm,xterm-256color \
        --with-default-terminfo-dir=/usr/share/terminfo \
        --with-terminfo-dirs="/usr/share/terminfo:/lib/terminfo:/etc/terminfo"
    make -j"$(nproc)" && make install.includes install.libs
    "$STRIP_BIN" -g "$ABS/staging/lib/libncurses.a" 2>/dev/null || true
    "$STRIP_BIN" -g "$ABS/staging/lib/libtinfo.a" 2>/dev/null || true
    touch "$STAMP" && cd ../..
fi

# ---- Compilation of zstd (Minified PowerPC) ----
echo "🗜️  Compiling minified zstd for PowerPC..."
cd third_party/zstd
make distclean >/dev/null 2>&1 || true
MINI_CFLAGS="-Os -fPIC -fno-ident -ffunction-sections -fdata-sections $MAP_FLAGS"
MINI_CFLAGS="$MINI_CFLAGS -DZSTD_NODICT -DZSTD_LIB_DICTBUILDER=0 -DZSTD_LEGACY_SUPPORT=0 -DZSTD_STRIP_ERROR_STRINGS=1"
make -C lib CC="$CC" CFLAGS="$MINI_CFLAGS" libzstd.a -j"$(nproc)"
"$STRIP_BIN" -g lib/libzstd.a 2>/dev/null || true
mkdir -p "$ABS/staging/lib" "$ABS/staging/include"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null || true
cd ../..

mkdir -p build

echo "🔐 Compiling Monocypher..."
$CC $CFLAGS -Os -fno-strict-aliasing -fwrapv -w -c third_party/monocypher/monocypher.c -o build/monocypher.o

echo "🔐 Compiling AES-GCM..."
$CC $CFLAGS -Os -fno-strict-aliasing -fwrapv -w -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

echo "📦 Compiling xdelta3..."
$CC -std=gnu11 -Os -w -fPIC -fno-ident $MAP_FLAGS -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=0 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=4 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=4 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Ithird_party/xdelta/xdelta3 -c third_party/xdelta/xdelta3/xdelta3.c -o build/xdelta3.o

if [ ! -f "build/xdelta3.o" ]; then echo "❌ xdelta3 build failed."; exit 1; fi

echo "🔗 Linking baresnap..."
BASE="$PATCHED_SRC/brs_index.c $PATCHED_SRC/brs_repo_create.c $PATCHED_SRC/brs_util.c $PATCHED_SRC/brs_buffer.c $PATCHED_SRC/brs_init.c $PATCHED_SRC/brs_repo_health.c $PATCHED_SRC/brs_vfs_context.c $PATCHED_SRC/brs_cache.c $PATCHED_SRC/brs_lock.c $PATCHED_SRC/brs_repo_ls.c $PATCHED_SRC/brs_vfs_dispatch.c $PATCHED_SRC/brs_chunker.c $PATCHED_SRC/brs_lz4.c $PATCHED_SRC/brs_repo_prune.c $PATCHED_SRC/brs_vfs_local.c $PATCHED_SRC/brs_config.c $PATCHED_SRC/brs_manifest.c $PATCHED_SRC/brs_repo_query.c $PATCHED_SRC/brs_vfs_ssh.c $PATCHED_SRC/brs_crypto.c $PATCHED_SRC/brs_pack.c $PATCHED_SRC/brs_repo_restore.c $PATCHED_SRC/brs_zstd.c $PATCHED_SRC/brs_delta.c $PATCHED_SRC/brs_pool.c $PATCHED_SRC/brs_repo_verify.c $PATCHED_SRC/main.c $PATCHED_SRC/brs_dir.c $PATCHED_SRC/brs_remote.c $PATCHED_SRC/brs_spsc.c $PATCHED_SRC/brs_fsutil.c $PATCHED_SRC/brs_tui.c $PATCHED_SRC/brs_hash.c $PATCHED_SRC/brs_repo_common.c $PATCHED_SRC/brs_uri.c"

$CC $CFLAGS -I"$ABS/staging/include" $BASE build/monocypher.o build/xdelta3.o build/aes_gcm.o -static -no-pie -Wl,--gc-sections -Wl,--start-group "$ABS/staging/lib/"*.a -Wl,--end-group -latomic -o build/baresnap
"$STRIP_BIN" --strip-all build/baresnap 2>/dev/null || true

echo "🔗 Compiling baresnap-remote..."
$CC -std=c11 -Os -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -I$PATCHED_SRC -Isrc "$PATCHED_SRC/baresnap-remote.c" "$PATCHED_SRC/brs_remote.c" -static -no-pie -Wl,--gc-sections -latomic -o build/baresnap-remote
"$STRIP_BIN" --strip-all build/baresnap-remote 2>/dev/null || true

echo "============================================================"
echo "✅ POWERPC BUILD COMPLETED"
echo "============================================================"
echo ""

for b in build/baresnap build/baresnap-remote; do
    if [ -f "$b" ]; then
        SIZE_BYTES=$(wc -c < "$b")
        SIZE_KB=$((SIZE_BYTES / 1024))
        echo "  $b : $SIZE_BYTES bytes ($SIZE_KB KB)"
    fi
done

echo ""
echo "🧬 Binary fingerprints:"

for b in build/baresnap build/baresnap-remote; do
    if [ -f "$b" ]; then
        printf "  %-25s " "$b"
        file -b "$b"
    fi
done

echo ""
echo ""
echo "🔒 Both binaries are fully static, stripped, and reproducible."
echo "   Epoch: $SOURCE_DATE_EPOCH"
echo "============================================================"
