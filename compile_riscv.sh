#!/bin/bash
set -euo pipefail

# ============================================================
# Local cross-compilation Toolchain configuration (RISC-V 64)
# ============================================================
CROSS_DIR="$HOME/cross-tools"
CC="riscv64-linux-musl-gcc"
STRIP_BIN="riscv64-linux-musl-strip"
TARGET="riscv64"
HOST_FLAG="--host=riscv64-linux-musl"

# Ensure that if the local toolchain exists, it is automatically added to the PATH
if [ -d "$CROSS_DIR/riscv64-linux-musl-cross/bin" ]; then
    export PATH="$CROSS_DIR/riscv64-linux-musl-cross/bin:$PATH"
fi

# Avoid leaking absolute paths (__FILE__) by mapping the current directory to a dot "."
MAP_FLAGS="-ffile-prefix-map=$(pwd)=."

# ============================================================
# Absolute determinism for SHA256 psychopaths (Reproducible Builds)
# MISMO EPOCH QUE EL SCRIPT x86_64 PARA DETERMINISMO ENTRE ARQUITECTURAS
# ============================================================
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-11791462664}"
export ZERO_AR_DATE=1

echo "⚙️ Target: RISC-V 64-bit (SBCs / VisionFive / Milk-V) - Forcing direct Cross-Compilation."
echo "🔒 Deterministic build frozen at epoch: $SOURCE_DATE_EPOCH"
echo ""

# ============================================================
# DEPENDENCY VERIFICATION AND AUTO-INSTALLATION
# ============================================================
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "⚠️  WARNING: Cross-compiler $CC not found in PATH."
    echo "⚙️  Starting automatic download of the spartan musl.cc toolchain..."
    mkdir -p "$CROSS_DIR"
    cd "$CROSS_DIR"
    if [ ! -f "riscv64-linux-musl-cross.tgz" ]; then
        echo "📥 Downloading ~100 MB of pure silicon from musl.cc..."
        wget -q --show-progress https://musl.cc/riscv64-linux-musl-cross.tgz
    fi
    echo "📦 Extracting cross-compilation environment..."
    tar -xzf riscv64-linux-musl-cross.tgz
    cd - >/dev/null
    # Force inclusion in the current script session
    export PATH="$CROSS_DIR/riscv64-linux-musl-cross/bin:$PATH"
fi

# We export it so sub-Makefiles inherit the RISC-V compiler obligatorily
export CC

if ! command -v "$STRIP_BIN" >/dev/null 2>&1; then
    echo "❌ ERROR: $STRIP_BIN not found even after deploying the toolchain."
    exit 1
fi

echo "✅ RISC-V tools ready for battle. Continuing..."
echo ""

# RISC-V layout: Forzamos alineamiento estricto por software (-mstrict-align)
ARCH_FLAGS="-march=rv64gc -mabi=lp64d -mstrict-align -fno-ident -fno-asynchronous-unwind-tables"

# Global CFLAGS seguras para evitar la corrupción de traps de memoria en Alpine
CFLAGS="-std=c11 -Wall -Wextra -O1 -fno-strict-aliasing -fwrapv -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t $ARCH_FLAGS"



ABS="$(pwd)/build"
mkdir -p build

# ---- 1. static ncurses 6.5 compiled for RISC-V ----
STAMP="$ABS/.ncurses_ok_${TARGET}"
if [ ! -f "$STAMP" ]; then
    cd third_party/ncurses-6.5
    echo "🧹 Cleaning ncurses to avoid x86_64 residues..."
    make distclean >/dev/null 2>&1 || true
    echo "🔨 Configuring ncurses cross-compiled for RISC-V..."
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

# ---- 2. monocypher (crypto core) ----
echo "🔐 Compiling Monocypher for RISC-V..."
$CC $CFLAGS -O1 -fno-strict-aliasing -fwrapv -w -c third_party/monocypher/monocypher.c -o build/monocypher.o

# ---- 2.5 aes-gcm in software/fallback mode for RISC-V (no Intel AES-NI) ----
echo "🔐 Compiling AES-256-GCM (software fallback) for RISC-V..."
$CC $CFLAGS -O1 -fno-strict-aliasing -fwrapv -w -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

# ---- 3. xdelta3 (delta encoding) ----
echo "📦 Compiling xdelta3 for RISC-V..."
$CC -std=gnu11 -O2 -w -fPIC -fno-ident $MAP_FLAGS \
    -DHAVE_CONFIG_H=0 \
    -DXD3_USE_LARGESIZET=1 \
    -DXD3_MAIN=0 \
    -DXD3_DEBUG=0 \
    -DREGRESSION_TEST=0 \
    -DSECONDARY_DJW=0 \
    -DSECONDARY_FGK=0 \
    -DSECONDARY_LZMA=0 \
    -DEXTERNAL_COMPRESSION=0 \
    -DVCDIFF_TOOLS=0 \
    -DSIZEOF_SIZE_T=8 \
    -DSIZEOF_UNSIGNED_INT=4 \
    -DSIZEOF_UNSIGNED_LONG=8 \
    -DSIZEOF_UNSIGNED_LONG_LONG=8 \
    -Dusize_t=uint64_t \
    -Dxoff_t=uint64_t \
    -Ithird_party/xdelta/xdelta3 \
    -c third_party/xdelta/xdelta3/xdelta3.c -o build/xdelta3.o

if [ ! -f "build/xdelta3.o" ]; then
    echo "❌ Real error: The file build/xdelta3.o was not created."
    exit 1
fi
echo "✅ Object build/xdelta3.o generated correctly."

# ---- 3.5 zstd (compression) ----
echo "🗜️  Compiling zstd for RISC-V..."
cd third_party/zstd
make distclean >/dev/null 2>&1 || true
make -C lib CC="$CC" CFLAGS="-O2 -fPIC -fno-ident $MAP_FLAGS" libzstd.a -j"$(nproc)"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null || true
cd ../..

# ---- 4. full binary (main baresnap with TUI) ----

echo "🔗 Linking the whole project together for RISC-V..."
BASE="src/brs_index.c \
src/brs_repo_create.c \
src/brs_util.c \
src/brs_buffer.c \
src/brs_init.c \
src/brs_repo_health.c \
src/brs_vfs_context.c \
src/brs_cache.c \
src/brs_lock.c \
src/brs_repo_ls.c \
src/brs_vfs_dispatch.c \
src/brs_chunker.c \
src/brs_lz4.c \
src/brs_repo_prune.c \
src/brs_vfs_local.c \
src/brs_config.c \
src/brs_manifest.c \
src/brs_repo_query.c \
src/brs_vfs_ssh.c \
src/brs_crypto.c \
src/brs_pack.c \
src/brs_repo_restore.c \
src/brs_zstd.c \
src/brs_delta.c \
src/brs_pool.c \
src/brs_repo_verify.c \
src/main.c \
src/brs_dir.c \
src/brs_remote.c \
src/brs_spsc.c \
src/brs_fsutil.c \
src/brs_tui.c \
src/brs_hash.c \
src/brs_repo_common.c \
src/brs_uri.c"

$CC $CFLAGS -I"$ABS/staging/include" \
    $BASE build/monocypher.o build/xdelta3.o build/aes_gcm.o \
    -static -no-pie -Wl,--gc-sections \
    -Wl,--start-group "$ABS/staging/lib/"*.a -Wl,--end-group \
    -o build/baresnap

if [ ! -f "build/baresnap" ]; then
    echo "❌ FATAL: build/baresnap was not created. Linking failed."
    exit 1
fi

# Apply RISC-V cross-strip
"$STRIP_BIN" --strip-all build/baresnap 2>/dev/null || true
echo "✅ Main binary: build/baresnap"

# ---- 5. remote binary (baresnap-remote agent) ----
echo "🔗 Compiling baresnap-remote for RISC-V..."
$CC -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc \
    src/baresnap-remote.c src/brs_remote.c \
    -static -no-pie -Wl,--gc-sections -o build/baresnap-remote



if [ ! -f "build/baresnap-remote" ]; then
    echo "❌ FATAL: build/baresnap-remote was not created. Linking failed."
    exit 1
fi

"$STRIP_BIN" --strip-all build/baresnap-remote 2>/dev/null || true
echo "✅ Remote agent: build/baresnap-remote"

# ============================================================
# FINAL SUMMARY
# ============================================================
echo ""
echo "============================================================"
echo "✅ RISC-V CROSS-COMPILATION COMPLETED SUCCESSFULLY"
echo "============================================================"
echo ""
file build/baresnap
file build/baresnap-remote
echo ""
ls -lh build/baresnap build/baresnap-remote
echo ""
echo "⚙️ Ready to deploy to any RISC-V 64-bit Single Board Computer."
echo "   scp build/baresnap build/baresnap-remote user@riscv-board:~/baresnap/"
echo ""
echo "🔒 Both binaries are fully static, stripped, and reproducible."
echo "   Epoch: $SOURCE_DATE_EPOCH"
echo "============================================================"

