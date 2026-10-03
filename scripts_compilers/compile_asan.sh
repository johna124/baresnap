#!/bin/bash
set -e
CC=gcc
SAN_FLAGS="-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls "

# Detectar AES-NI real para ASan
AES_FLAGS=""
if grep -q " aes " /proc/cpuinfo 2>/dev/null; then
    AES_FLAGS=""
    echo "🚀 [SAN] AES-NI detectado en CPU"
else
    echo "⚠️  [SAN] CPU sin AES-NI. Omitiendo flags AES hardware."
fi

# Detectar banderas de ZSTD del sistema mediante pkg-config
if pkg-config --exists libzstd; then
    ZSTD_CFLAGS=$(pkg-config --cflags libzstd)
    ZSTD_LIBS=$(pkg-config --libs libzstd)
    echo "📦 [SYS] Zstandard detectada en el sistema"
else
    echo "❌ [ERROR] No se encontró libzstd-dev en el sistema. Instálala antes de continuar."
    exit 1
fi

CFLAGS="-std=c11 -Wall -Wextra $SAN_FLAGS $AES_FLAGS $ZSTD_CFLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t"

mkdir -p build_san

echo "== Compilando monocypher (ASan) =="
$CC $SAN_FLAGS -std=c11 -w -c third_party/monocypher/monocypher.c -o build_san/monocypher.o

echo "== Compilando aes_gcm (ASan + AES-NI) =="
$CC $SAN_FLAGS $AES_FLAGS -std=c11 -w -Ithird_party/aes-gcm -c third_party/aes-gcm/aes_gcm.c -o build_san/aes_gcm.o

echo "== Compilando xdelta3 (ASan) =="
$CC $SAN_FLAGS -std=gnu11 -w -fPIC \
    -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 \
    -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 \
    -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 \
    -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 \
    -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dusize_t=uint64_t -Dxoff_t=uint64_t \
    -Ithird_party/xdelta/xdelta3 \
    -c third_party/xdelta/xdelta3/xdelta3.c -o build_san/xdelta3.o

# === ELIMINADO: Ya no compilamos localmente third_party/zstd ===

echo "== Enlazando binario ASan =="
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

# Aquí cambiamos la ruta del .a por la variable $ZSTD_LIBS del sistema
$CC $CFLAGS \
    $BASE \
    build_san/monocypher.o build_san/aes_gcm.o build_san/xdelta3.o \
    $ZSTD_LIBS \
    $NCURSES_LIBS -lpthread -lm \
    -o build_san/baresnap

echo "== Compilando baresnap-remote (Limpio para SSH) =="
# Compilamos TODO desde las fuentes directas sin arrastrar objetos con ASan
$CC -std=c11 -O2 -Wall -Wextra -Isrc -Ithird_party/aes-gcm \
    src/baresnap-remote.c \
    src/brs_remote.c \
    third_party/monocypher/monocypher.c \
    third_party/aes-gcm/aes_gcm.c \
    $ZSTD_LIBS -lpthread -lm \
    -o build_san/baresnap-remote


echo ""
echo "✅ build_san/baresnap listo utilizando la ZSTD del sistema"
echo "   Ejecuta: ./stress_san.sh"

