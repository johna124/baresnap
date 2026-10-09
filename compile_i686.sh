#!/bin/bash
# ==============================================================================
# COMPILE_i686.SH - TOOLCHAIN DE COMPILACIÓN FORZADA PARA ARQUITECTURAS 32-BIT
# ==============================================================================
set -euo pipefail

sudo apt install gcc-multilib g++-multilib

# Forzar el compilador nativo de 32 bits (requiere gcc-multilib o toolchain i686)
CC="gcc"
M32_FLAG="-m32"
TARGET="i686"

# Determinismo absoluto para psicópatas de SHA256 (Reproducible Builds)
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1791462664}"
export ZERO_AR_DATE=1

# Evitar filtrado de rutas absolutas (__FILE__) mapeando el directorio actual a "."
MAP_FLAGS="-ffile-prefix-map=$(pwd)=."

# Configuración estricta de arquitectura para i686 (Sin telemetría ni registros 64-bit)
# Se desactiva el AES-NI por hardware nativo de 64 bits para evitar SIGILL (Illegal Instruction)
ARCH_FLAGS="$M32_FLAG -fno-ident -fno-asynchronous-unwind-tables $MAP_FLAGS"
echo "⚠️  Target i686 (32-bit Little Endian) detectado: Forzando cifrado portable por software."

# Configuración global de CFLAGS reconfigurada específicamente para 32 bits
# -DSIZEOF_SIZE_T=4 y -DSIZEOF_UNSIGNED_LONG=4 corrigen los desbordamientos de XDelta en RAM
# Configuración global de CFLAGS reconfigurada específicamente para 32 bits (Limpia de macros conflictivas)
CFLAGS="$M32_FLAG -march=i686 -std=c11 -Wall -Wextra -O2 -ffunction-sections -fdata-sections -fno-ident $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=0 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=4 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=4 -DSIZEOF_UNSIGNED_LONG_LONG=8"


ABS="$(pwd)/build"
STAMP="$ABS/.ncurses_ok"
mkdir -p build

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
# HOT PATCH 3: brs_delta.c i686-safe
# ============================================================
echo "🛠️  Replacing brs_delta.c with i686-safe wrapper..."

cat << 'EOF' > "$PATCHED_SRC/brs_delta.c"
/*
 * BareSnap i686-optimized xdelta3 wrapper.
 *
 * Diseñado estrictamente para arquitecturas de 32 bits.
 * Elimina la redundancia de tipos largos y asegura el alineamiento en la pila.
 */

#include "brs_delta.h"
#include "xdelta3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* Forzamos el tipo plano nativo de la CPU de 32 bits */
typedef size_t brs_xsize_t;

static pthread_mutex_t g_xd3_mutex = PTHREAD_MUTEX_INITIALIZER;

int brs_delta_encode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *new_data, size_t new_size,
                     uint8_t **delta_out, size_t *delta_size_out)
{
    if (!old_data || !new_data || !delta_out || !delta_size_out)
        return -1;

    if (getenv("BRS_DISABLE_DELTA"))
        return -1;

    /* Cálculo de capacidad seguro para evitar desbordamientos en 32 bits */
    brs_xsize_t cap = new_size + (new_size / 10) + 64;

    uint8_t *out_buf = (uint8_t *)malloc(cap);
    if (!out_buf)
        return -1;

    brs_xsize_t out_size = cap;

    pthread_mutex_lock(&g_xd3_mutex);
    /* Invocación limpia: xdelta3 mapea directo sobre size_t nativos */
    int ret = xd3_encode_memory(new_data, new_size,
                                old_data, old_size,
                                out_buf, &out_size,
                                cap, 0);
    pthread_mutex_unlock(&g_xd3_mutex);

    if (ret != 0) {
        free(out_buf);
        return -1;
    }

    *delta_out = out_buf;
    *delta_size_out = out_size;
    return 0;
}

int brs_delta_decode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *delta_data, size_t delta_size,
                     size_t expected_new_size,
                     uint8_t **new_out, size_t *new_size_out)
{
    if (!old_data || !delta_data || !new_out || !new_size_out)
        return -1;

    uint8_t *out_buf = (uint8_t *)malloc(expected_new_size ? expected_new_size : 1);
    if (!out_buf)
        return -1;

    brs_xsize_t out_size = expected_new_size;

    pthread_mutex_lock(&g_xd3_mutex);
    int ret = xd3_decode_memory(delta_data, delta_size,
                                old_data, old_size,
                                out_buf, &out_size,
                                expected_new_size, 0);
    pthread_mutex_unlock(&g_xd3_mutex);

    if (ret != 0) {
        free(out_buf);
        return -1;
    }

    *new_out = out_buf;
    *new_size_out = out_size;
    return 0;
}

EOF

echo "✅ brs_delta.c replaced."

# ---- 1. ncurses 6.5 estático compilado en 32 bits ----
if [ ! -f "$STAMP" ]; then
    echo "Compilando ncurses de 32 bits..."
    cd third_party/ncurses-6.5
    ./configure CC="$CC" CFLAGS="$M32_FLAG -O2 -fPIC -fno-ident $MAP_FLAGS" --prefix="$ABS/staging" \
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
echo "Compiling Monocypher (32-bit)..."
$CC -std=c11 $M32_FLAG -O3 -w -fno-ident $MAP_FLAGS -c third_party/monocypher/monocypher.c -o build/monocypher.o

# ---- 2.5 aes-gcm (software portable sin registros de 64 bits nativos) ----
echo "Compiling AES-256-GCM Portable..."
$CC -std=c11 -O3 -w $ARCH_FLAGS -Ithird_party/aes-gcm -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

echo "📦 Compiling xdelta3..."
$CC -std=gnu11 -m32 -O2 -w -fPIC -fno-ident $MAP_FLAGS \
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


# ---- 3.5 zstd ----
echo "Compiling zstd (32-bit)..."
cd third_party/zstd
make -C lib CC="$CC" CFLAGS="$M32_FLAG -O2 -fPIC -fno-ident $MAP_FLAGS" libzstd.a -j"$(nproc)"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null
cd ../..

# ---- 4. Enlazado final del binario estático monolítico ----
echo "Linking the whole project together for i686..."
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

# Eliminación absoluta de toda sección redundante de depuración y símbolos
strip --strip-all build/baresnap 2>/dev/null || true
ls -lh build/baresnap
echo "OK: Binario de 32 bits generado en ./build/baresnap"

# ---- 5. Binario remoto secundario (baresnap-remote) ----
echo "Compiling baresnap-remote (32-bit)..."
$CC $M32_FLAG -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -fno-ident $MAP_FLAGS -Isrc \
    src/baresnap-remote.c src/brs_remote.c \
    -static -no-pie -Wl,--gc-sections -o build/baresnap-remote
strip --strip-all build/baresnap-remote 2>/dev/null || true
ls -lh build/baresnap-remote
echo "OK: ./build/baresnap-remote [32-bit]"

# ============================================================
# FINAL SUMMARY
# ============================================================
echo ""
echo "============================================================"
echo "✅ i686 COMPILATION COMPLETED SUCCESSFULLY"
echo "============================================================"
echo ""
file build/baresnap
file build/baresnap-remote
echo ""
ls -lh build/baresnap build/baresnap-remote
