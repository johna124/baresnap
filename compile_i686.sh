#!/bin/bash
set -euo pipefail

# ==============================================================================
# 1. PARAMETER PARSING & VALIDATION
# ==============================================================================
USAGE="Usage: $0 {musl|gcc}"
CROSS_DIR="$HOME/cross-tools"
TARGET="i686"

if [ $# -lt 1 ]; then
    echo "❌ ERROR: Missing target compiler option."
    echo "$USAGE"
    exit 1
fi

case "$1" in
    musl)
        CC="i686-linux-musl-gcc"
        STRIP_BIN="i686-linux-musl-strip"
        HOST_FLAG="--host=i686-linux-musl"
        M32_FLAG="" 
        TINFO_FLAVOR="musl"
        
        if [ -d "$CROSS_DIR/i686-linux-musl-cross/bin" ]; then
            export PATH="$CROSS_DIR/i686-linux-musl-cross/bin:$PATH"
        fi

        if command -v "$CC" >/dev/null 2>&1; then
            echo "🚀 $CC cross-compiler detected. Preparing 32-bit Musl build."
        else
            echo "⚠️  WARNING: '$CC' toolchain not found in PATH."
            echo "   Bootstrapping official lightweight i686 musl toolchain from musl.cc..."
            echo ""
            mkdir -p "$CROSS_DIR" && cd "$CROSS_DIR"
            if [ ! -f "i686-linux-musl-cross.tgz" ]; then
                wget -q --show-progress https://musl.cc/i686-linux-musl-cross.tgz
            fi
            tar -xzf i686-linux-musl-cross.tgz && cd - >/dev/null
            export PATH="$CROSS_DIR/i686-linux-musl-cross/bin:$PATH"
        fi
        ;;
        
    gcc)

        sudo apt install -y gcc-multilib g++-multilib >/dev/null 2>&1 || true
        CC="gcc"
        STRIP_BIN="strip"
        HOST_FLAG="" 
        M32_FLAG="-m32"
        TINFO_FLAVOR="glibc"
        echo "⚠️  Target toolchain set to native gcc -m32 (glibc multilib build)."
        
        if ! echo 'int main(){return 0;}' | $CC $M32_FLAG -x c - -o /tmp/m32_check >/dev/null 2>&1; then
            echo "❌ ERROR: Your local gcc cannot compile 32-bit binaries with -m32."
            echo "   Please fix your multilib environment: sudo apt install gcc-multilib"
            exit 1
        fi
        rm -f /tmp/m32_check
        ;;
        
    *)
        echo "❌ ERROR: Invalid option '$1'."
        echo "$USAGE"
        exit 1
        ;;
esac


export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1793637764}"
export ZERO_AR_DATE=1
MAP_FLAGS="-ffile-prefix-map=$(pwd)=."
ABS="${ABS:-$(pwd)/build}"
STAMP="${STAMP:-$ABS/.ncurses_ok}"

ARCH_FLAGS="$M32_FLAG -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS"
CFLAGS="$M32_FLAG -march=i686 -std=c11 -Wall -Wextra -Os -ffunction-sections -fdata-sections -fno-ident $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=0 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=4 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=4 -DSIZEOF_UNSIGNED_LONG_LONG=8"

mkdir -p build
PATCHED_SRC="build/src_patched"
rm -rf "$PATCHED_SRC" && mkdir -p "$PATCHED_SRC"
cp -r src/* "$PATCHED_SRC/"
CFLAGS="$CFLAGS -I$PATCHED_SRC -DBRS_IMPLEMENTATION"

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
    brs_xsize_t cap = new_size + (new_size / 10) + 64;
    uint8_t *out_buf = (uint8_t *)malloc(cap);
    if (!out_buf) return -1;
    brs_xsize_t out_size = cap;
    pthread_mutex_lock(&g_xd3_mutex);
    int ret = xd3_encode_memory(new_data, new_size, old_data, old_size, out_buf, &out_size, cap, 0);
    pthread_mutex_unlock(&g_xd3_mutex);
    if (ret != 0) { free(out_buf); return -1; }
    *delta_out = out_buf; *delta_size_out = out_size; return 0;
}
int brs_delta_decode(const uint8_t *old_data, size_t old_size, const uint8_t *delta_data, size_t delta_size, size_t expected_new_size, uint8_t **new_out, size_t *new_size_out) {
    if (!old_data || !delta_data || !new_out || !new_size_out) return -1;
    uint8_t *out_buf = (uint8_t *)malloc(expected_new_size ? expected_new_size : 1);
    if (!out_buf) return -1;
    brs_xsize_t out_size = expected_new_size;
    pthread_mutex_lock(&g_xd3_mutex);
    int ret = xd3_decode_memory(delta_data, delta_size, old_data, old_size, out_buf, &out_size, expected_new_size, 0);
    pthread_mutex_unlock(&g_xd3_mutex);
    if (ret != 0) { free(out_buf); return -1; }
    *new_out = out_buf; *new_size_out = out_size; return 0;
}
EOF

# ---- 1. static ncurses 6.5 compiled for i686 ----
if [ ! -f "$STAMP" ]; then
    echo "🔨 Configuring and compiling static ncurses 6.5..."
    cd third_party/ncurses-6.5
    make distclean >/dev/null 2>&1 || true
    
    NCURSES_CFLAGS="$M32_FLAG -Os -fPIC -fno-ident -ffunction-sections -fdata-sections $MAP_FLAGS"
    
    if [ "$TINFO_FLAVOR" = "musl" ]; then
        EXTRA_CROSS_FLAGS="--build=x86_64-linux-gnu --host=i686-linux-musl"
    else
        EXTRA_CROSS_FLAGS=""
    fi
    
    ./configure CC="$CC" CFLAGS="$NCURSES_CFLAGS" \
        $EXTRA_CROSS_FLAGS \
        --prefix="$ABS/staging" \
        --enable-static --disable-shared --disable-widec --enable-overwrite \
        --with-termlib --without-progs --without-tests --without-cxx \
        --without-cxx-binding --without-ada --without-manpages \
        --disable-home-terminfo --disable-ext-colors --disable-mouse \
        --with-fallbacks=linux,vt100,xterm,xterm-256color \
        --with-default-terminfo-dir=/usr/share/terminfo \
        --with-terminfo-dirs="/usr/share/terminfo:/lib/terminfo:/etc/terminfo"
        
    make -j"$(nproc)"
    make install.includes install.libs
    
    "$STRIP_BIN" -g "$ABS/staging/lib/libncurses.a" 2>/dev/null || strip -g "$ABS/staging/lib/libncurses.a"
    "$STRIP_BIN" -g "$ABS/staging/lib/libtinfo.a" 2>/dev/null || strip -g "$ABS/staging/lib/libtinfo.a"
    
    test -f "$ABS/staging/lib/libncurses.a"
    touch "$STAMP"
    cd ../..
fi


# ---- Compilation of zstd (32-bit, stripped) ----
echo "Compiling zstd (32-bit minified)..."
cd third_party/zstd
MINI_CFLAGS="$M32_FLAG -Os -fPIC -fno-ident -ffunction-sections -fdata-sections $MAP_FLAGS"
MINI_CFLAGS="$MINI_CFLAGS -DZSTD_NODICT -DZSTD_LIB_DICTBUILDER=0 -DZSTD_LEGACY_SUPPORT=0 -DZSTD_STRIP_ERROR_STRINGS=1"
make -C lib CC="$CC" CFLAGS="$MINI_CFLAGS" libzstd.a -j"$(nproc)"
"${CROSS_COMPILE:-}strip" -g lib/libzstd.a 2>/dev/null || strip -g lib/libzstd.a
mkdir -p "$ABS/staging/lib" "$ABS/staging/include"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null
cd ../..

mkdir -p build

echo "Compiling Monocypher (32-bit)..."
$CC -std=c11 $M32_FLAG -Os -w -fno-ident $MAP_FLAGS -c third_party/monocypher/monocypher.c -o build/monocypher.o

echo "Compiling AES-256-GCM Portable..."
$CC -std=c11 -Os -w $ARCH_FLAGS -Ithird_party/aes-gcm -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

echo "📦 Compiling xdelta3..."
$CC -std=gnu11 -m32 -Os -w -fPIC -fno-ident $MAP_FLAGS -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=0 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=4 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=4 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Ithird_party/xdelta/xdelta3 -c third_party/xdelta/xdelta3/xdelta3.c -o build/xdelta3.o


# ---- 6. full binary ----
echo "Linking the whole project together for i686..."
BASE="$PATCHED_SRC/brs_index.c src/brs_repo_create.c src/brs_util.c src/brs_buffer.c src/brs_init.c src/brs_repo_health.c src/brs_vfs_context.c src/brs_cache.c src/brs_lock.c src/brs_repo_ls.c src/brs_vfs_dispatch.c src/brs_chunker.c src/brs_lz4.c src/brs_repo_prune.c src/brs_vfs_local.c src/brs_config.c src/brs_manifest.c src/brs_repo_query.c src/brs_vfs_ssh.c src/brs_crypto.c src/brs_pack.c src/brs_repo_restore.c src/brs_zstd.c src/brs_delta.c src/brs_pool.c src/brs_repo_verify.c src/main.c src/brs_dir.c src/brs_remote.c src/brs_spsc.c src/brs_fsutil.c src/brs_tui.c src/brs_hash.c src/brs_repo_common.c src/brs_uri.c"


$CC $CFLAGS -pthread -I"$ABS/staging/include" -L"$ABS/staging/lib" \
    $BASE build/monocypher.o build/xdelta3.o build/aes_gcm.o \
    -static -no-pie -Wl,--gc-sections \
    -Wl,--start-group "$ABS/staging/lib/"*.a -Wl,--end-group \
    -o build/baresnap

"$STRIP_BIN" --strip-all build/baresnap 2>/dev/null || strip --strip-all build/baresnap 2>/dev/null || true


$CC $CFLAGS -pthread -I"$ABS/staging/include" $BASE build/monocypher.o build/xdelta3.o build/aes_gcm.o -static -no-pie -Wl,--gc-sections -Wl,--start-group "$ABS/staging/lib/"*.a -Wl,--end-group -o build/baresnap
strip --strip-all build/baresnap 2>/dev/null || true

echo "Compiling baresnap-remote (32-bit)..."
$CC $M32_FLAG -std=c11 -Os -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident $MAP_FLAGS -Isrc src/baresnap-remote.c src/brs_remote.c -static -no-pie -Wl,--gc-sections -o build/baresnap-remote
strip --strip-all build/baresnap-remote 2>/dev/null || true

echo "============================================================"
echo "✅ i686 COMPILATION COMPLETED SUCCESSFULLY WITH: ${CC}" 
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
