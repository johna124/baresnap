#!/bin/bash
set -euo pipefail

# ==============================================================================
# 1. PARAMETER PARSING & VALIDATION
# ==============================================================================
USAGE="Usage: $0 {musl|gcc}"

# Check if at least one argument is passed
if [ $# -lt 1 ]; then
    echo "❌ ERROR: Missing target compiler option."
    echo "$USAGE"
    exit 1
fi

# Assign compiler based on the first argument
case "$1" in
    musl)
        if command -v musl-gcc >/dev/null 2>&1; then
            CC="musl-gcc"
            TINFO_FLAVOR="musl"
            echo "🚀 Target toolchain set to musl-gcc. Preparing ultra-lightweight build."
        else
            echo "❌ ERROR: 'musl-gcc' (musl-tools) is not installed on this system."
            echo "   Please run: sudo apt update && sudo apt install -y musl-tools"
            exit 1
        fi
        ;;
    gcc)
        CC="gcc"
        TINFO_FLAVOR="glibc"
        echo "⚠️  Target toolchain set to native gcc. Preparing standard glibc build."
        ;;
    *)
        echo "❌ ERROR: Invalid option '$1'."
        echo "$USAGE"
        exit 1
        ;;
esac

export CC

# ==============================================================================
# 2. GLOBAL CFLAGS & ENVIRONMENT CONFIGURATION
# ==============================================================================
MAP_FLAGS="-ffile-prefix-map=$(pwd)=."
ABS="${ABS:-$(pwd)/build}"
STAMP="${STAMP:-$ABS/.ncurses_ok_x86_64_${TINFO_FLAVOR}}"

# Reproducible builds configuration
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1793637764}"
export ZERO_AR_DATE=1

# Architecture optimization flags
ARCH_FLAGS="-fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS"

# Hardware encryption auto-detection (AES-NI)
if uname -m | grep -qE "x86_64|amd64"; then
    CPUFLAGS="$(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null || true)"
    if echo " $CPUFLAGS " | grep -q " aes " && \
       echo " $CPUFLAGS " | grep -q " pclmulqdq " && \
       echo " $CPUFLAGS " | grep -q " sse4_1 " && \
       echo " $CPUFLAGS " | grep -q " ssse3 "; then
        echo "🚀 x86_64 with AES-NI detected: enabling hardware AES acceleration"
    else
        echo "⚠️  x86_64 without AES-NI detected: compiling portable fallback software AES"
    fi
fi

# Global optimized CFLAGS targeting space (-Os) and section pruning for the linker
CFLAGS="-std=c11 -Wall -Wextra -Os -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t"

# ---- 1. static ncurses 6.5 compiled with selected toolchain ----
if [ ! -f "$STAMP" ]; then
    echo "🔨 Configuring and compiling static ncurses 6.5..."
    cd third_party/ncurses-6.5
    make distclean >/dev/null 2>&1 || true
    
    NCURSES_CFLAGS="-Os -fPIC -fno-ident -ffunction-sections -fdata-sections $MAP_FLAGS"
    
    # Configure with minified footprint flags (no mouse, no extended colors, minimal fallbacks)
    ./configure CC="$CC" CFLAGS="$NCURSES_CFLAGS" --prefix="$ABS/staging" \
        --enable-static --disable-shared --disable-widec --enable-overwrite \
        --with-termlib --without-progs --without-tests --without-cxx \
        --without-cxx-binding --without-ada --without-manpages \
        --disable-home-terminfo --disable-ext-colors --disable-mouse \
        --with-fallbacks=linux,vt100,xterm,xterm-256color \
        --with-default-terminfo-dir=/usr/share/terminfo \
        --with-terminfo-dirs="/usr/share/terminfo:/lib/terminfo:/etc/terminfo"
        
    make -j"$(nproc)"
    make install.includes install.libs
    
    "${CROSS_COMPILE:-}strip" -g "$ABS/staging/lib/libncurses.a" 2>/dev/null || strip -g "$ABS/staging/lib/libncurses.a"
    "${CROSS_COMPILE:-}strip" -g "$ABS/staging/lib/libtinfo.a" 2>/dev/null || strip -g "$ABS/staging/lib/libtinfo.a"
    
    test -f "$ABS/staging/lib/libncurses.a"
    touch "$STAMP"
    cd ../..
fi

# Ensure output directory structure exists
mkdir -p build

# ---- 2. monocypher ----
echo "🔐 Compiling Monocypher..."
$CC -std=c11 -Os -w -fno-ident $MAP_FLAGS -c third_party/monocypher/monocypher.c -o build/monocypher.o

# ---- 2.5 aes-gcm with AES-NI ----
echo "🔐 Compiling AES-256-GCM..."
$CC -std=c11 -Os -w $ARCH_FLAGS -Ithird_party/aes-gcm -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

# ---- 3. xdelta ----
echo "📦 Compiling xdelta3..."
$CC -std=gnu11 -Os -w -fPIC -fno-ident $MAP_FLAGS \
    -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t \
    -Ithird_party/xdelta/xdelta3 -c third_party/xdelta/xdelta3/xdelta3.c -o build/xdelta3.o

if [ ! -f "build/xdelta3.o" ]; then
    echo "❌ FATAL: build/xdelta3.o was not created."
    exit 1
fi

# ---- 3.5 zstd ----
echo "🗜️  Compiling minified zstd (Up to Level 22 Support)..."
cd third_party/zstd
MINI_CFLAGS="-Os -fPIC -fno-ident -ffunction-sections -fdata-sections $MAP_FLAGS"
MINI_CFLAGS="$MINI_CFLAGS -DZSTD_NODICT -DZSTD_LIB_DICTBUILDER=0 -DZSTD_LEGACY_SUPPORT=0 -DZSTD_STRIP_ERROR_STRINGS=1"

make -C lib CC="$CC" CFLAGS="$MINI_CFLAGS" libzstd.a -j"$(nproc)"
"${CROSS_COMPILE:-}strip" -g lib/libzstd.a 2>/dev/null || strip -g lib/libzstd.a

mkdir -p "$ABS/staging/lib" "$ABS/staging/include"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null
cd ../..

# ---- 4. full binary ----
echo "🔗 Linking the whole project together..."
BASE="src/brs_index.c src/brs_repo_create.c src/brs_util.c src/brs_buffer.c src/brs_init.c src/brs_repo_health.c src/brs_vfs_context.c src/brs_cache.c src/brs_lock.c src/brs_repo_ls.c src/brs_vfs_dispatch.c src/brs_chunker.c src/brs_lz4.c src/brs_repo_prune.c src/brs_vfs_local.c src/brs_config.c src/brs_manifest.c src/brs_repo_query.c src/brs_vfs_ssh.c src/brs_crypto.c src/brs_pack.c src/brs_repo_restore.c src/brs_zstd.c src/brs_delta.c src/brs_pool.c src/brs_repo_verify.c src/main.c src/brs_dir.c src/brs_remote.c src/brs_spsc.c src/brs_fsutil.c src/brs_tui.c src/brs_hash.c src/brs_repo_common.c src/brs_uri.c"

$CC $CFLAGS -pthread -I"$ABS/staging/include" \
    $BASE build/monocypher.o build/xdelta3.o build/aes_gcm.o \
    -static -no-pie -Wl,--gc-sections \
    -Wl,--start-group "$ABS/staging/lib/"*.a -Wl,--end-group \
    -o build/baresnap

strip --strip-all build/baresnap 2>/dev/null || true

# ---- 5. remote binary (baresnap-remote) ----
echo "🔗 Compiling baresnap-remote agent..."
$CC -std=c11 -Os -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc \
    src/baresnap-remote.c src/brs_remote.c \
    -static -no-pie -Wl,--gc-sections -o build/baresnap-remote

strip --strip-all build/baresnap-remote 2>/dev/null || true

# ============================================================
# FINAL OUTPUT DISPLAY
# ============================================================
echo ""
echo "============================================================"
echo "✅ x86_64 COMPLETED SUCCESSFULLY WITH: ${CC}"
echo "============================================================"
echo ""

# 1. Exact size footprint calculation
for b in build/baresnap build/baresnap-remote; do
    if [ -f "$b" ]; then
        SIZE_BYTES=$(wc -c < "$b")
        SIZE_KB=$((SIZE_BYTES / 1024))
        echo "  $b : $SIZE_BYTES bytes ($SIZE_KB KB)"
    fi
done

echo ""
echo "🧬 Binary fingerprints:"

# 2. Binary metadata analysis
for b in build/baresnap build/baresnap-remote; do
    if [ -f "$b" ]; then
        printf "  %-25s " "$b"
        file -b "$b"
    fi
done

echo ""
echo "============================================================"

