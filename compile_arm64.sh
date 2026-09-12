#!/bin/bash
set -euo pipefail

# Local cross-compilation Toolchain configuration
CROSS_DIR="$HOME/cross-tools"
CC="aarch64-linux-musl-gcc"
STRIP_BIN="aarch64-linux-musl-strip"
TARGET="arm64"
HOST_FLAG="--host=aarch64-linux-musl"

# Ensure that if the local toolchain exists, it is automatically added to the PATH
if [ -d "$CROSS_DIR/aarch64-linux-musl-cross/bin" ]; then
export PATH="$CROSS_DIR/aarch64-linux-musl-cross/bin:$PATH"
fi

# Avoid leaking absolute paths (__FILE__) by mapping the current directory to a dot "."
MAP_FLAGS="-ffile-prefix-map=$(pwd)=."

# Absolute determinism for SHA256 psychopaths (Reproducible Builds)
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"
export ZERO_AR_DATE=1

echo "🍓 Target: ARM64 (Raspberry Pi) - Forcing direct Cross-Compilation."
# ============================================================
# DEPENDENCY VERIFICATION AND AUTO-INSTALLATION
# ============================================================
if ! command -v "$CC" >/dev/null 2>&1; then
echo "⚠️  WARNING: Cross-compiler $CC not found in PATH."
echo "⚙️  Starting automatic download of the spartan musl.cc toolchain..."
mkdir -p "$CROSS_DIR"
cd "$CROSS_DIR"
if [ ! -f "aarch64-linux-musl-cross.tgz" ]; then
echo "📥 Downloading ~100 MB of pure silicon from musl.cc..."
wget -q --show-progress https://musl.cc/aarch64-linux-musl-cross.tgz
fi
echo "📦 Extracting cross-compilation environment..."
tar -xzf aarch64-linux-musl-cross.tgz
cd - >/dev/null

# Force inclusion in the current script session
export PATH="$CROSS_DIR/aarch64-linux-musl-cross/bin:$PATH"
fi

# We export it so sub-Makefiles inherit the ARM64 compiler obligatorily
export CC
if ! command -v "$STRIP_BIN" >/dev/null 2>&1; then
echo "❌ ERROR: $STRIP_BIN not found even after deploying the toolchain."
exit 1
fi
echo "✅ ARM64 tools ready for battle. Continuing..."
echo ""

# ARM64 does not use Intel AES-NI. Pure C fallback
ARCH_FLAGS="-march=armv8-a -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS"
# Global CFLAGS with prefix mapping for ARM64
CFLAGS="-std=c11 -Wall -Wextra -O2 -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t"
ABS="$(pwd)/build"
mkdir -p build

# ---- 1. static ncurses 6.5 compiled for ARM64 ----
STAMP="$ABS/.ncurses_ok_${TARGET}"
if [ ! -f "$STAMP" ]; then
cd third_party/ncurses-6.5
echo "🧹 Cleaning ncurses to avoid x86_64 residues..."
make distclean >/dev/null 2>&1 || true
echo "🔨 Configuring ncurses cross-compiled for ARM64..."
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

# ---- 2. monocypher ----
echo "Compiling Monocypher for ARM64..."
$CC -std=c11 -O3 -w -fno-ident $MAP_FLAGS -c third_party/monocypher/monocypher.c -o build/monocypher.o

# ---- 2.5 aes-gcm in software/fallback mode for ARM64 ----
echo "Compiling AES-256-GCM for ARM64..."
$CC -std=c11 -O3 -w $ARCH_FLAGS -Ithird_party/aes-gcm -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

# ---- 3. xdelta ----
echo "Compiling xdelta3 for ARM64..."
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
-c third_party/xdelta/xdelta3/xdelta3.c -o build/xdelta3.o || true
if [ -f "build/xdelta3.o" ]; then
echo "✅ Object build/xdelta3.o generated correctly."
else
echo "❌ Real error: The file build/xdelta3.o was not created."
exit 1
fi

# ---- 3.5 zstd ----
echo "Compiling zstd for ARM64..."
cd third_party/zstd
make distclean >/dev/null 2>&1 || true
make -C lib CC="$CC" CFLAGS="-O2 -fPIC -fno-ident $MAP_FLAGS" libzstd.a -j"$(nproc)"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null
cd ../..

# ---- 4. full binary ----
echo "Linking the whole project together for ARM64..."
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

# Apply ARM64 cross-strip
"$STRIP_BIN" --strip-all build/baresnap 2>/dev/null || true
file build/baresnap

# ---- 5. remote binary (aresnap-remote) ----
echo "Compiling baresnap-remote for ARM64..."
$CC -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc \
src/baresnap-remote.c src/brs_remote.c \
-static -no-pie -Wl,--gc-sections -o build/baresnap-remote
"$STRIP_BIN" --strip-all build/baresnap-remote 2>/dev/null || true
file build/baresnap-remote
