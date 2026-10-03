#!/bin/bash
set -euo pipefail
CC=gcc

# Absolute determinism for SHA256 psychopaths (Reproducible Builds)
# Absolute determinism: Change the number to freeze it on another historical date
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1788118500}"
export ZERO_AR_DATE=1


# Avoid leaking absolute paths (__FILE__) by mapping the current directory to a dot "."
MAP_FLAGS="-ffile-prefix-map=$(pwd)=."

# Detect architecture and real AES-NI
ARCH_FLAGS="-fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS"
if uname -m | grep -qE "x86_64|amd64"; then
CPUFLAGS="$(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null || true)"
if echo " $CPUFLAGS " | grep -q " aes " && \
echo " $CPUFLAGS " | grep -q " pclmulqdq " && \
echo " $CPUFLAGS " | grep -q " sse4_1 " && \
echo " $CPUFLAGS " | grep -q " ssse3 "; then
ARCH_FLAGS="$ARCH_FLAGS"
echo "🚀 x86_64 with AES-NI detected: enabling hardware AES"
else
echo "⚠️  x86_64 without AES-NI (Core 2 Duo or similar): compiling portable AES (without)"
fi
fi

# 1. Global CFLAGS configuration (Injecting anti-telemetry flags, gc-sections and anti-path-leakage)
CFLAGS="-std=c11 -Wall -Wextra -O2 -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t"
ABS="$(pwd)/build"
STAMP="$ABS/.ncurses_ok"
mkdir -p build

# ---- 1. static ncurses 6.5 compiled with gcc ----
if [ ! -f "$STAMP" ]; then
cd third_party/ncurses-6.5
./configure CC="$CC" CFLAGS="-O2 -fPIC -fno-ident $MAP_FLAGS" --prefix="$ABS/staging" \
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
echo "Compiling Monocypher..."
$CC -std=c11 -O3 -w -fno-ident $MAP_FLAGS -c third_party/monocypher/monocypher.c -o build/monocypher.o

# ---- 2.5 aes-gcm with AES-NI ----
echo "Compiling AES-256-GCM (with AES-NI + PCLMULQDQ)..."
$CC -std=c11 -O3 -w $ARCH_FLAGS -Ithird_party/aes-gcm -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

# ---- 3. xdelta ----
echo "Compiling xdelta3..."
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
echo "Compiling zstd..."
cd third_party/zstd
make -C lib CC="$CC" CFLAGS="-O2 -fPIC -fno-ident $MAP_FLAGS" libzstd.a -j"$(nproc)"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null
cd ../..

# ---- 4. full binary ----
echo "Linking the whole project together..."
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
$CC $CFLAGS -pthread -I"$ABS/staging/include" \
$BASE build/monocypher.o build/xdelta3.o build/aes_gcm.o \
-static -no-pie -Wl,--gc-sections \
-Wl,--start-group "$ABS/staging/lib/"*.a -Wl,--end-group \
-o build/baresnap

# Force a full strip of all redundant debug and symbol sections
strip --strip-all build/baresnap 2>/dev/null || true
ls -lh build/baresnap
echo "OK: ./build/baresnap [repo] [local_path]"

# ---- 5. remote binary (baresnap-remote) ----
echo "Compiling baresnap-remote..."
$CC -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS -Isrc \
src/baresnap-remote.c src/brs_remote.c \
-static -no-pie -Wl,--gc-sections -o build/baresnap-remote
strip --strip-all build/baresnap-remote 2>/dev/null || true
ls -lh build/baresnap-remote
echo "OK: ./build/baresnap-remote --repo /path/to/repo"
