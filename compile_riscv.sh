#!/bin/bash
set -euo pipefail

# ============================================================
# Toolchain Config: Híbrido GCC (Infraestructura) + Clang (BareSnap)
# ============================================================
CROSS_DIR="$HOME/cross-tools"
TARGET="riscv64-linux-musl"
HOST_FLAG="--host=riscv64-linux-musl"

# Aseguramos inclusión del toolchain local en el PATH
if [ -d "$CROSS_DIR/riscv64-linux-musl-cross/bin" ]; then
    export PATH="$CROSS_DIR/riscv64-linux-musl-cross/bin:$PATH"
fi

CC_GCC="riscv64-linux-musl-gcc"
STRIP_BIN="riscv64-linux-musl-strip"
SYSROOT="$CROSS_DIR/riscv64-linux-musl-cross/riscv64-linux-musl"

# RUTA FORENSE DE COMPONENTES GCC (Solución al fallo crtbeginT.o)
GCC_VERSION=$("$CC_GCC" -dumpversion)
GCC_LIB_DIR="$CROSS_DIR/riscv64-linux-musl-cross/lib/gcc/riscv64-linux-musl/$GCC_VERSION"

# Verificación de requisitos en el sistema Host
if ! command -v clang >/dev/null 2>&1; then
    echo "❌ ERROR: Clang no está instalado en tu máquina host."
    echo "👉 Instálalo ejecutando: sudo apt install clang"
    exit 1
fi

if ! command -v "$CC_GCC" >/dev/null 2>&1; then
    echo "⚠️  WARNING: Cross-compiler $CC_GCC no encontrado en el PATH."
    echo "⚙️  Instalando automáticamente el toolchain musl.cc..."
    mkdir -p "$CROSS_DIR"
    cd "$CROSS_DIR"
    if [ ! -f "riscv64-linux-musl-cross.tgz" ]; then
        echo "📥 Descargando toolchain desde musl.cc..."
        wget -q --show-progress https://musl.cc
    fi
    echo "📦 Extrayendo entorno de compilación cruzada..."
    tar -xzf riscv64-linux-musl-cross.tgz
    cd - >/dev/null
    export PATH="$CROSS_DIR/riscv64-linux-musl-cross/bin:$PATH"
    GCC_VERSION=$("$CC_GCC" -dumpversion)
    GCC_LIB_DIR="$CROSS_DIR/riscv64-linux-musl-cross/lib/gcc/riscv64-linux-musl/$GCC_VERSION"
fi

# Evitamos fugas de rutas absolutas mediante prefix mapping
MAP_FLAGS="-ffile-prefix-map=$(pwd)=."

# Determinismo absoluto para compilaciones reproducibles
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1788118500}"
export ZERO_AR_DATE=1

echo "⚙️ Target: RISC-V 64-bit (GCC/Clang Hybrid Toolchain Deployment)"
echo "🔒 Deterministic build frozen at epoch: $SOURCE_DATE_EPOCH"
echo "🛡️  GCC Runtime Objects path: $GCC_LIB_DIR"
echo ""

ABS="$(pwd)/build"
mkdir -p build

# ---- 1. static ncurses 6.5 (Compilado con GCC para evitar el exit 77 de Autotools) ----
STAMP="$ABS/.ncurses_ok_${TARGET}"
if [ ! -f "$STAMP" ]; then
    cd third_party/ncurses-6.5
    echo "🧹 Cleaning ncurses..."
    rm -f config.cache
    make distclean >/dev/null 2>&1 || true
    echo "🔨 Configuring and compiling ncurses via GCC..."
    ./configure CC="$CC_GCC" CFLAGS="-O2 -fPIC -fno-ident $MAP_FLAGS" \
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
echo "🗜️  Compiling zstd for RISC-V..."
cd third_party/zstd
make distclean >/dev/null 2>&1 || true
make -C lib CC="$CC_GCC" CFLAGS="-O2 -fPIC -fno-ident $MAP_FLAGS" libzstd.a -j"$(nproc)"
cp lib/libzstd.a "$ABS/staging/lib/"
cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "$ABS/staging/include/" 2>/dev/null || true
cd ../..

# ============================================================
# CONFIGURACIÓN DE FLAGS EXCLUSIVAS PARA CLANG / LLVM
# ============================================================
# -mstrict-align obliga a Clang a resolver accesos desalineados de memoria criptográfica en software
CLANG_ARCH="-target riscv64-linux-musl --sysroot=$SYSROOT -march=rv64gc -mabi=lp64d -mstrict-align -fno-ident"

CLANG_CFLAGS="-std=c11 -Wall -Wextra -O1 -fno-strict-aliasing -fwrapv -ffunction-sections -fdata-sections $MAP_FLAGS -Isrc -Ithird_party/monocypher -Ithird_party/xdelta/xdelta3 -Ithird_party/aes-gcm -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dxoff_t=uint64_t $CLANG_ARCH"

echo "🔐 Compiling Cripto Core (Monocypher) with Clang..."
clang $CLANG_CFLAGS -c third_party/monocypher/monocypher.c -o build/monocypher.o

echo "🔐 Compiling AES-256-GCM fallback with Clang..."
clang $CLANG_CFLAGS -c third_party/aes-gcm/aes_gcm.c -o build/aes_gcm.o

echo "📦 Compiling xdelta3 with Clang (Forcing global type safety)..."
clang -std=gnu11 -O2 -w -fPIC $CLANG_ARCH $MAP_FLAGS \
    -include stdint.h -Dusize_t=uint64_t \
    -DHAVE_CONFIG_H=0 -DXD3_USE_LARGESIZET=1 -DXD3_MAIN=0 -DXD3_DEBUG=0 \
    -DREGRESSION_TEST=0 -DSECONDARY_DJW=0 -DSECONDARY_FGK=0 -DSECONDARY_LZMA=0 \
    -DEXTERNAL_COMPRESSION=0 -DVCDIFF_TOOLS=0 -DSIZEOF_SIZE_T=8 -DSIZEOF_UNSIGNED_INT=4 \
    -DSIZEOF_UNSIGNED_LONG=8 -DSIZEOF_UNSIGNED_LONG_LONG=8 -Dxoff_t=uint64_t \
    -Ithird_party/xdelta/xdelta3 \
    -c third_party/xdelta/xdelta3/xdelta3.c -o build/xdelta3.o

# ---- 4. full binary (Construido e inyectado secuencialmente por Clang) ----
echo "🔗 Linking the whole project together with CLANG..."
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

# Enlazado final cruzado estático inyectando rutas explícitas de objetos y librerías (-B y -L) de GCC
clang $CLANG_CFLAGS -I"$ABS/staging/include" -B"$GCC_LIB_DIR" -L"$GCC_LIB_DIR" -fuse-ld=bfd \
    $BASE build/monocypher.o build/xdelta3.o build/aes_gcm.o \
    -static -no-pie \
    -Wl,--start-group "$ABS/staging/lib/"*.a -Wl,--end-group \
    -o build/baresnap

if [ ! -f "build/baresnap" ]; then
    echo "❌ FATAL: build/baresnap was not created. Linking failed."
    exit 1
fi

"$STRIP_BIN" --strip-all build/baresnap 2>/dev/null || true
echo "✅ Main binary compiled with CLANG: build/baresnap"

# ---- 5. remote binary (baresnap-remote agent) ----
echo "🔗 Compiling baresnap-remote with Clang..."
clang -std=c11 -O2 -Wall -Wextra $CLANG_ARCH $MAP_FLAGS -Isrc -B"$GCC_LIB_DIR" -L"$GCC_LIB_DIR" -fuse-ld=bfd \
    src/baresnap-remote.c src/brs_remote.c \
    -static -no-pie -o build/baresnap-remote

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
echo "🎯 CLANG/LLVM RISC-V HYBRID BUILD COMPLETED SUCCESSFULLY"
echo "============================================================"
echo ""
file build/baresnap
file build/baresnap-remote
echo ""
ls -lh build/baresnap build/baresnap-remote
echo ""
echo "⚙️ Ready to stress-test layout structures in QEMU."

