#!/bin/bash
set -euo pipefail

CC=gcc

# ============================================================
# Reproducibility
# ============================================================
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1788118500}"
export ZERO_AR_DATE=1

# ============================================================
# Dependencies
# ============================================================
MISSING=0

if ! command -v gcc >/dev/null 2>&1; then
    echo "❌ ERROR: gcc is not installed."
    echo "   Install it with: sudo apt install build-essential"
    MISSING=1
fi

if ! command -v gdb >/dev/null 2>&1; then
    echo "⚠️  WARNING: gdb is not installed."
    echo "   Install it with: sudo apt install gdb"
fi

echo ""
echo "Recommended to install also:"
echo "   sudo apt install libncurses-dev libzstd-dev"
echo ""

if [ "$MISSING" -eq 1 ]; then
    echo ""
    echo "❌ Missing critical dependencies. Aborting."
    exit 1
fi

echo "✅ Dependencies verified. Continuing..."
echo ""


# ============================================================
# Base flags
# ============================================================
MAP_FLAGS="-ffile-prefix-map=$(pwd)=."

ARCH_FLAGS="-fno-ident $MAP_FLAGS"

if uname -m | grep -qE "x86_64|amd64"; then
    CPUFLAGS="$(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null || true)"

    if echo " $CPUFLAGS " | grep -q " aes " && \
       echo " $CPUFLAGS " | grep -q " pclmulqdq " && \
       echo " $CPUFLAGS " | grep -q " sse4_1 " && \
       echo " $CPUFLAGS " | grep -q " ssse3 "; then
        echo "🚀 x86_64 with AES-NI detected: enabling hardware AES"
    else
        echo "⚠️  x86_64 without AES-NI: compiling portable AES"
    fi
fi

# ============================================================
# Debug flags (Enabled ASAN + UBSAN for memory safety checks)
# ============================================================
OPT_DEBUG="-O0 -g -ggdb3 -fno-omit-frame-pointer -fno-inline -fsanitize=address,undefined"

CFLAGS="-std=c11 -Wall -Wextra $OPT_DEBUG $ARCH_FLAGS \
-Isrc \
-Ithird_party/monocypher \
-Ithird_party/xdelta/xdelta3 \
-Ithird_party/aes-gcm \
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
-Dxoff_t=uint64_t"

ABS="$(pwd)/build"
mkdir -p "$ABS"

# ============================================================
# Monocypher
# ============================================================
echo "Compiling Monocypher..."
$CC -std=c11 $OPT_DEBUG -w -fno-ident $MAP_FLAGS \
    -c third_party/monocypher/monocypher.c \
    -o build/monocypher.o

# ============================================================
# AES-GCM
# ============================================================
echo "Compiling AES-256-GCM..."
$CC -std=c11 $OPT_DEBUG -w $ARCH_FLAGS \
    -Ithird_party/aes-gcm \
    -c third_party/aes-gcm/aes_gcm.c \
    -o build/aes_gcm.o

# ============================================================
# xdelta
# ============================================================
echo "Compiling xdelta3..."
$CC -std=gnu11 $OPT_DEBUG -w -fPIC -fno-ident $MAP_FLAGS \
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
    -c third_party/xdelta/xdelta3/xdelta3.c \
    -o build/xdelta3.o

if [ ! -f "build/xdelta3.o" ]; then
    echo "❌ Real error: The file build/xdelta3.o was not created."
    exit 1
fi

echo "✅ Object build/xdelta3.o generated correctly."

# ============================================================
# Main Sources
# ============================================================
SOURCES="src/brs_index.c \
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

# ============================================================
# Minimal External Libraries
# ============================================================
# If your SSH implementation is custom, DO NOT add -lssh2.
# If your crypto is custom / monocypher / aes-gcm, DO NOT add -lcrypto.
#
# If the linker asks for specific symbols, add only what is necessary.
# For example:
#
#   undefined reference to libssh2_*  -> add -lssh2
#   undefined reference to EVP_*      -> add -lcrypto
#
LIBS="-lncurses -lzstd -lpthread"

# ============================================================
# Final Linker
# ============================================================
echo "Linking the whole project together (Debug + ASAN + UBSAN)..."

$CC $CFLAGS \
    $SOURCES \
    build/monocypher.o \
    build/xdelta3.o \
    build/aes_gcm.o \
    -rdynamic \
    $LIBS \
    -o build/baresnap

echo "✅ Debug binary built at build/baresnap"

# ============================================================
# Remote binary
# ============================================================
echo "Compiling baresnap-remote..."

$CC -std=c11 $OPT_DEBUG -Wall -Wextra $MAP_FLAGS -Isrc \
    src/baresnap-remote.c \
    src/brs_remote.c \
    -rdynamic \
    -lpthread \
    -o build/baresnap-remote

ls -lh build/baresnap build/baresnap-remote

cat << 'EOF'

=========================================================
✅ DEBUG COMPILATION (WITH GDB SUPPORT) COMPLETED
=========================================================

👉 To debug with GDB when it hangs (in another SSH terminal):

   1. Leave the program hanging. DO NOT use pkill.
   2. Execute:

      sudo gdb -p $(pgrep -x baresnap) \
        -ex 'set pagination off' \
        -ex 'info threads' \
        -ex 'thread apply all bt full' \
        -ex 'quit' > /tmp/backtrace.txt

   3. Review the output:

      less /tmp/backtrace.txt

EOF

