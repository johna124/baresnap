#!/bin/bash
set -euo pipefail

CROSS_DIR="$HOME/cross-tools"
CC="riscv64-linux-musl-gcc"
STRIP_BIN="riscv64-linux-musl-strip"
TARGET="riscv64"
HOST_FLAG="--host=riscv64-linux-musl"

if [ -d "$CROSS_DIR/riscv64-linux-musl-cross/bin" ]; then
    export PATH="$CROSS_DIR/riscv64-linux-musl-cross/bin:$PATH"
fi

MAP_FLAGS="-ffile-prefix-map=$(pwd)=."
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1793637764}"
export ZERO_AR_DATE=1

echo "⚙️ Target: RISC-V 64-bit (SBCs / VisionFive / Milk-V)"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "⚠️ Cross-compiler not found. Deploying musl.cc toolchain..."
    mkdir -p "$CROSS_DIR" && cd "$CROSS_DIR"
    if [ ! -f "riscv64-linux-musl-cross.tgz" ]; then
        wget -q --show-progress https://musl.cc
    fi
    tar -xzf riscv64-linux-musl-cross.tgz && cd - >/dev/null
    export PATH="$CROSS_DIR/riscv64-linux-musl-cross/bin:$PATH"
fi
export CC

if ! command -v "$STRIP_BIN" >/dev/null 2>&1; then
    echo "❌ ERROR: $STRIP_BIN not found." && exit 1
fi

ABS="${ABS:-$(pwd)/build}"
STAMP="${STAMP:-$ABS/.ncurses_ok_${TARGET}}"
mkdir -p build

# Architecture-specific flags for RISC-V with safe alignment
ARCH_FLAGS="-march=rv64gc -mabi=lp64d -mstrict-align -fno-ident -fno-asynchronous-unwind-tables"

CFLAGS="-std=c11 -Wall -Wextra -Os -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t $ARCH_FLAGS"

# ---- 1. static ncurses 6.5 compiled for RISC-V (Minified) ----
if [ ! -f "$STAMP" ]; then
    cd third_party/ncurses-6.5
    echo "🧹 Cleaning ncurses to avoid residues..."
    make distclean >/dev/null 2>&1 || true
    echo "🔨 Configuring ncurses cross-compiled for RISC-V..."
    
    NCURSES_CFLAGS="-Os -fPIC -fno-ident -ffunction-sections -fdata-sections $MAP_FLAGS $ARCH_FLAGS"
    
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

# ---- 3.5 zstd (Minified for RISC-V) ----
echo "🗜️  Compiling zstd for RISC-V..."
cd third_party/zstd
make distclean >/dev/null 2>&1 || true

MINI_CFLAGS="-Os -fPIC -fno-ident -ffunction-sections -fdata-sections $MAP_FLAGS $ARCH_FLAGS"
MINI_CFLAGS="$MINI_CFLAGS -DZSTD_NODICT -DZSTD_LIB_DICTBUILDER=0 -DZSTD_LEGACY_SUPPORT=0 -DZSTD_STRIP_ERROR_STRINGS=1"

make -C lib CC="$CC" CFLAGS="$MINI_CFLAGS" libzstd.a -j"$(nproc)"
"$STRIP_BIN" -g lib/libzstd.a 2>/dev/null || true

mkdir -p "$ABS/staging/lib" "$ABS/staging/include"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null || true
cd ../..

mkdir -p build

echo "🔐 Compiling Monocypher for RISC-V..."
$CC $CFLAGS -Os -w -c third_party/monocypher/monocypher.c -o build/monocypher.o

echo "🔐 Compiling AES-256-GCM (software fallback) for RISC-V..."
$CC $CFLAGS -Os -w -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

echo "📦 Compiling xdelta3 for RISC-V..."
$CC -std=gnu11 -Os -w -fPIC -fno-ident $MAP_FLAGS $ARCH_FLAGS \
    -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t \
    -Ithird_party/xdelta/xdelta3 -c third_party/xdelta/xdelta3/xdelta3.c -o build/xdelta3.o

if [ ! -f "build/xdelta3.o" ]; then echo "❌ xdelta3 build failed."; exit 1; fi

echo "🔗 Linking the whole project together for RISC-V..."
BASE="src/brs_index.c src/brs_repo_create.c src/brs_util.c src/brs_buffer.c src/brs_init.c src/brs_repo_health.c src/brs_vfs_context.c src/brs_cache.c src/brs_lock.c src/brs_repo_ls.c src/brs_vfs_dispatch.c src/brs_chunker.c src/brs_lz4.c src/brs_repo_prune.c src/brs_vfs_local.c src/brs_config.c src/brs_manifest.c src/brs_repo_query.c src/brs_vfs_ssh.c src/brs_crypto.c src/brs_pack.c src/brs_repo_restore.c src/brs_zstd.c src/brs_delta.c src/brs_pool.c src/brs_repo_verify.c src/main.c src/brs_dir.c src/brs_remote.c src/brs_spsc.c src/brs_fsutil.c src/brs_tui.c src/brs_hash.c src/brs_repo_common.c src/brs_uri.c"

$CC $CFLAGS -I"$ABS/staging/include" $BASE build/monocypher.o build/xdelta3.o build/aes_gcm.o -static -no-pie -Wl,--gc-sections -Wl,--start-group "$ABS/staging/lib/"*.a -Wl,--end-group -o build/baresnap
"$STRIP_BIN" --strip-all build/baresnap 2>/dev/null || true

echo "🔗 Compiling baresnap-remote for RISC-V..."
$CC -std=c11 -Os -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS $ARCH_FLAGS -Isrc src/baresnap-remote.c src/brs_remote.c -static -no-pie -Wl,--gc-sections -o build/baresnap-remote
"$STRIP_BIN" --strip-all build/baresnap-remote 2>/dev/null || true

echo "============================================================"
echo "✅ RISC-V CROSS-COMPILATION COMPLETED SUCCESSFULLY"
echo "============================================================"

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
