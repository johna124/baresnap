#!/bin/bash
set -euo pipefail

# ============================================================
# Toolchain PowerPC32
# ============================================================
CROSS_DIR="$HOME/cross-tools"
CC="powerpc-linux-musl-gcc"
STRIP_BIN="powerpc-linux-musl-strip"
TARGET="powerpc"
HOST_FLAG="--host=powerpc-linux-musl"

if [ -d "$CROSS_DIR/powerpc-linux-musl-cross/bin" ]; then
    export PATH="$CROSS_DIR/powerpc-linux-musl-cross/bin:$PATH"
fi

MAP_FLAGS="-ffile-prefix-map=$(pwd)=."

export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1791462664}"
export ZERO_AR_DATE=1

echo "⚙️ Target: PowerPC 32-bit (PPC Classic / Big Endian)"
echo "🔒 Deterministic build frozen at epoch: $SOURCE_DATE_EPOCH"
echo ""

# ============================================================
# Toolchain check
# ============================================================
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "⚠️  WARNING: Cross-compiler $CC not found in PATH."
    mkdir -p "$CROSS_DIR"
    cd "$CROSS_DIR"
    if [ ! -f "powerpc-linux-musl-cross.tgz" ]; then
        echo "📥 Downloading toolchain from musl.cc..."
        wget -q --show-progress https://musl.cc/powerpc-linux-musl-cross.tgz
    fi
    tar -xzf powerpc-linux-musl-cross.tgz
    cd - >/dev/null
    export PATH="$CROSS_DIR/powerpc-linux-musl-cross/bin:$PATH"
fi
export CC

if ! command -v "$STRIP_BIN" >/dev/null 2>&1; then
    echo "❌ ERROR: $STRIP_BIN not found."
    exit 1
fi

echo "✅ PowerPC tools ready."
echo ""

# ============================================================
# CFLAGS base
# IMPORTANTE:
#   - Quitamos -include src/ppc32_fixes.h
#   - Ponemos XD3_USE_LARGESIZET=0 para PPC32
# ============================================================
CFLAGS="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Os -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=0 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=4 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=4 -DSIZEOF_UNSIGNED_LONG_LONG=8 -DSYS_getrandom=-1"

# ============================================================
# PATCHED SRC
# ============================================================
PATCHED_SRC="build/src_patched"
rm -rf "$PATCHED_SRC"
mkdir -p "$PATCHED_SRC"
cp -r src/* "$PATCHED_SRC/"

# Ahora el include parcheado manda
CFLAGS="$CFLAGS -I$PATCHED_SRC -DBRS_IMPLEMENTATION"

ABS="$(pwd)/build"
mkdir -p build

# ============================================================
# HOT PATCH 1: brs_repo_restore.c defensivo
# ============================================================
RESTORE_C="$PATCHED_SRC/brs_repo_restore.c"

if [ -f "$RESTORE_C" ]; then
    echo "🛠️  Hot-patching brs_repo_restore.c..."

    # Usa entries_len en todas partes
    sed -i 's/snap\.entry_count/snap.entries_len/g' "$RESTORE_C"

    # No borrar el fichero maestro de hardlinks
    sed -i 's/(void)unlink(src_p);/(void)0; \/* HOTPATCH: preserve hardlink master *\//g' "$RESTORE_C"

    # No crear placeholder "shared content"
    sed -i 's/int fd_origen = open(src_p, O_CREAT | O_WRONLY | O_TRUNC, 0644);/int fd_origen = -1; \/* HOTPATCH: no placeholder *\//g' "$RESTORE_C"
    sed -i 's/if (fd_origen >= 0) {/if (0) { \/* HOTPATCH disabled placeholder *\//g' "$RESTORE_C"

    echo "✅ brs_repo_restore.c hot-patched."
fi

# ============================================================
# HOT PATCH 2: prune con modo opcional BRS_PRUNE_NO_REWRITE=1
# ============================================================
PRUNE_C="$PATCHED_SRC/brs_repo_prune.c"

if [ -f "$PRUNE_C" ]; then
    echo "🛠️  Hot-patching brs_repo_prune.c..."

    sed -i 's/if (dead_ratio < BRS_REWRITE_THRESHOLD)/if (getenv("BRS_PRUNE_NO_REWRITE") || dead_ratio < BRS_REWRITE_THRESHOLD)/' "$PRUNE_C"

    echo "✅ brs_repo_prune.c hot-patched."
fi

# ============================================================
# HOT PATCH 3: brs_delta.c PPC32-safe
# ============================================================
echo "🛠️  Replacing brs_delta.c with PPC32-safe wrapper..."

cat << 'EOF' > "$PATCHED_SRC/brs_delta.c"
/*
 * BareSnap PPC32-safe xdelta3 wrapper.
 *
 * Hot patch generado para evitar el fallo de restore delta en PPC32.
 * Usa tipos de tamaño explícitos y loguea errores de decodificación.
 */

#include "brs_delta.h"
#include "xdelta3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#if defined(XD3_USE_LARGESIZET) && (XD3_USE_LARGESIZET == 1)
typedef unsigned long long brs_xsize_t;
#else
typedef size_t brs_xsize_t;
#endif

static pthread_mutex_t g_xd3_mutex = PTHREAD_MUTEX_INITIALIZER;

int brs_delta_encode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *new_data, size_t new_size,
                     uint8_t **delta_out, size_t *delta_size_out)
{
    if (!old_data || !new_data || !delta_out || !delta_size_out)
        return -1;

    /* Debug/escape: permite forzar snapshots full con BRS_DISABLE_DELTA=1 */
    if (getenv("BRS_DISABLE_DELTA"))
        return -1;

    brs_xsize_t cap =
        (brs_xsize_t)new_size +
        (brs_xsize_t)(new_size / 10) +
        (brs_xsize_t)64;

    if (cap > (brs_xsize_t)SIZE_MAX)
        return -1;

    uint8_t *out_buf = (uint8_t *)malloc((size_t)cap);
    if (!out_buf)
        return -1;

    brs_xsize_t out_size = cap;

    pthread_mutex_lock(&g_xd3_mutex);
    int ret = xd3_encode_memory(new_data, (brs_xsize_t)new_size,
                                old_data, (brs_xsize_t)old_size,
                                out_buf, &out_size,
                                cap, 0);
    pthread_mutex_unlock(&g_xd3_mutex);

    if (ret != 0) {
        FILE *f = fopen("/tmp/brs_delta_encode_fail.log", "a");
        if (f) {
            fprintf(f,
                    "xd3_encode_memory ret=%d old_size=%zu new_size=%zu cap=%zu\n",
                    ret, old_size, new_size, (size_t)cap);
            fclose(f);
        }
        free(out_buf);
        return -1;
    }

    *delta_out = out_buf;
    *delta_size_out = (size_t)out_size;
    return 0;
}

int brs_delta_decode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *delta_data, size_t delta_size,
                     size_t expected_new_size,
                     uint8_t **new_out, size_t *new_size_out)
{
    if (!old_data || !delta_data || !new_out || !new_size_out)
        return -1;

    if (expected_new_size > SIZE_MAX)
        return -1;

    uint8_t *out_buf =
        (uint8_t *)malloc(expected_new_size ? expected_new_size : 1);
    if (!out_buf)
        return -1;

    brs_xsize_t out_size = (brs_xsize_t)expected_new_size;

    pthread_mutex_lock(&g_xd3_mutex);
    int ret = xd3_decode_memory(delta_data, (brs_xsize_t)delta_size,
                                old_data, (brs_xsize_t)old_size,
                                out_buf, &out_size,
                                (brs_xsize_t)expected_new_size, 0);
    pthread_mutex_unlock(&g_xd3_mutex);

    if (ret != 0) {
        FILE *f = fopen("/tmp/brs_delta_decode_fail.log", "a");
        if (f) {
            fprintf(f,
                    "xd3_decode_memory ret=%d delta_size=%zu old_size=%zu expected_new_size=%zu\n",
                    ret, delta_size, old_size, expected_new_size);
            fclose(f);
        }
        free(out_buf);
        return -1;
    }

    *new_out = out_buf;
    *new_size_out = (size_t)out_size;
    return 0;
}
EOF

echo "✅ brs_delta.c replaced."

# ============================================================
# 1. ncurses
# ============================================================
STAMP="$ABS/.ncurses_ok_${TARGET}"
if [ ! -f "$STAMP" ]; then
    cd third_party/ncurses-6.5
    make distclean >/dev/null 2>&1 || true
    ./configure CC="$CC" CFLAGS="-O2 -fPIC -fno-ident $MAP_FLAGS" \
        "$HOST_FLAG" \
        --prefix="$ABS/staging" \
        --enable-static --disable-shared --disable-widec --enable-overwrite \
        --with-termlib --without-progs --without-tests --without-cxx \
        --without-cxx-binding --without-ada --without-manpages \
        --with-default-terminfo-dir=/usr/share/terminfo \
        --with-terminfo-dirs="/usr/share/terminfo:/lib/terminfo:/etc/terminfo"
    make -j"$(nproc)"
    make install.includes install.libs
    test -f "$ABS/staging/lib/libncurses.a"
    touch "$STAMP"
    cd ../..
fi

# ============================================================
# 2. monocypher
# ============================================================
echo "🔐 Compiling Monocypher..."
$CC $CFLAGS -O1 -fno-strict-aliasing -fwrapv -w -c third_party/monocypher/monocypher.c -o build/monocypher.o

# ============================================================
# 2.5 aes-gcm
# ============================================================
echo "🔐 Compiling AES-GCM..."
$CC $CFLAGS -O1 -fno-strict-aliasing -fwrapv -w -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

# ============================================================
# 3. xdelta3
# IMPORTANTE: aquí también XD3_USE_LARGESIZET=0
# ============================================================
echo "📦 Compiling xdelta3..."
$CC -std=gnu11 -O2 -w -fPIC -fno-ident $MAP_FLAGS \
    -DHAVE_CONFIG_H=0 \
    -DXD3_USE_LARGESIZET=0 \
    -DXD3_MAIN=0 \
    -DXD3_DEBUG=0 \
    -DREGRESSION_TEST=0 \
    -DSECONDARY_DJW=0 \
    -DSECONDARY_FGK=0 \
    -DSECONDARY_LZMA=0 \
    -DEXTERNAL_COMPRESSION=0 \
    -DVCDIFF_TOOLS=0 \
    -DSIZEOF_SIZE_T=4 \
    -DSIZEOF_UNSIGNED_INT=4 \
    -DSIZEOF_UNSIGNED_LONG=4 \
    -DSIZEOF_UNSIGNED_LONG_LONG=8 \
    -Ithird_party/xdelta/xdelta3 \
    -c third_party/xdelta/xdelta3/xdelta3.c -o build/xdelta3.o

if [ ! -f "build/xdelta3.o" ]; then
    echo "❌ Real error: build/xdelta3.o was not created."
    exit 1
fi

# ============================================================
# 3.5 zstd
# ============================================================
echo "🗜️  Compiling zstd..."
cd third_party/zstd
make distclean >/dev/null 2>&1 || true
make -C lib CC="$CC" CFLAGS="-O2 -fPIC -fno-ident $MAP_FLAGS" libzstd.a -j"$(nproc)"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null || true
cd ../..

# ============================================================
# 4. baresnap main binary
# ============================================================
echo "🔗 Linking baresnap..."

BASE="$PATCHED_SRC/brs_index.c \
$PATCHED_SRC/brs_repo_create.c \
$PATCHED_SRC/brs_util.c \
$PATCHED_SRC/brs_buffer.c \
$PATCHED_SRC/brs_init.c \
$PATCHED_SRC/brs_repo_health.c \
$PATCHED_SRC/brs_vfs_context.c \
$PATCHED_SRC/brs_cache.c \
$PATCHED_SRC/brs_lock.c \
$PATCHED_SRC/brs_repo_ls.c \
$PATCHED_SRC/brs_vfs_dispatch.c \
$PATCHED_SRC/brs_chunker.c \
$PATCHED_SRC/brs_lz4.c \
$PATCHED_SRC/brs_repo_prune.c \
$PATCHED_SRC/brs_vfs_local.c \
$PATCHED_SRC/brs_config.c \
$PATCHED_SRC/brs_manifest.c \
$PATCHED_SRC/brs_repo_query.c \
$PATCHED_SRC/brs_vfs_ssh.c \
$PATCHED_SRC/brs_crypto.c \
$PATCHED_SRC/brs_pack.c \
$PATCHED_SRC/brs_repo_restore.c \
$PATCHED_SRC/brs_zstd.c \
$PATCHED_SRC/brs_delta.c \
$PATCHED_SRC/brs_pool.c \
$PATCHED_SRC/brs_repo_verify.c \
$PATCHED_SRC/main.c \
$PATCHED_SRC/brs_dir.c \
$PATCHED_SRC/brs_remote.c \
$PATCHED_SRC/brs_spsc.c \
$PATCHED_SRC/brs_fsutil.c \
$PATCHED_SRC/brs_tui.c \
$PATCHED_SRC/brs_hash.c \
$PATCHED_SRC/brs_repo_common.c \
$PATCHED_SRC/brs_uri.c"

$CC $CFLAGS -I"$ABS/staging/include" \
    $BASE build/monocypher.o build/xdelta3.o build/aes_gcm.o \
    -static -no-pie -Wl,--gc-sections \
    -Wl,--start-group "$ABS/staging/lib/"*.a -Wl,--end-group \
    -latomic \
    -o build/baresnap

if [ ! -f "build/baresnap" ]; then
    echo "❌ FATAL: build/baresnap was not created."
    exit 1
fi

"$STRIP_BIN" --strip-all build/baresnap 2>/dev/null || true
echo "✅ Main binary: build/baresnap"

# ============================================================
# 5. baresnap-remote
# ============================================================
echo "🔗 Compiling baresnap-remote..."

$CC -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -I$PATCHED_SRC -Isrc \
    "$PATCHED_SRC/baresnap-remote.c" "$PATCHED_SRC/brs_remote.c" \
    -static -no-pie -Wl,--gc-sections \
    -latomic \
    -o build/baresnap-remote

if [ ! -f "build/baresnap-remote" ]; then
    echo "❌ FATAL: build/baresnap-remote was not created."
    exit 1
fi

"$STRIP_BIN" --strip-all build/baresnap-remote 2>/dev/null || true
echo "✅ Remote agent: build/baresnap-remote"

echo ""
echo "============================================================"
echo "✅ POWERPC BUILD COMPLETED"
echo "============================================================"
file build/baresnap build/baresnap-remote
sha256sum build/baresnap build/baresnap-remote
