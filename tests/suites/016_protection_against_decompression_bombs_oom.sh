# ============================================================
# 16. Protection against decompression bombs (OOM)
# ============================================================
section "16. Protection against decompression bombs (OOM)"

REPO16="$WORK/repo16"
SRC16="$WORK/src16"
OUT16="$WORK/out16"

assert_ok "init" "$BARESNAP" init "$REPO16"
mkdir -p "$SRC16"
dd if=/dev/zero of="$SRC16/bomb.bin" bs=1M count=32 status=none
assert_ok "create" "$BARESNAP" create "$REPO16" "$SRC16"

SNAP16=$(get_latest_snap "$REPO16")

# ---------------------------------------------------------------------------
# Detect if the binary is instrumented with ASan/TSan/UBSan.
# Sanitizers require tens of MB of shadow memory at startup,
# so `ulimit -v 8192` causes them to crash before executing a single instruction.
# ---------------------------------------------------------------------------
IS_SANITIZED=0
if ldd "$BARESNAP" 2>/dev/null | grep -qE "libasan|libtsan|libubsan"; then
    IS_SANITIZED=1
elif nm "$BARESNAP" 2>/dev/null | grep -qE "__asan_init|__tsan_init|__ubsan"; then
    IS_SANITIZED=1
fi

# ---------------------------------------------------------------------------
# Detect cross-arch emulation (QEMU user-mode, binfmt_misc, etc.)
#
# QEMU needs to create internal threads with stacks of ~8 MB each.
# With ulimit -v 8192, pthread_create() returns EAGAIN and QEMU calls
# abort() before executing a single guest instruction:
#
#   qemu: qemu_thread_create: Resource temporarily unavailable
#   Aborted (core dumped)
#
# Detection compares the ELF binary's architecture with the host's.
# If they do not match, emulation is necessarily involved.
# ---------------------------------------------------------------------------
IS_EMULATED=0
if [ -f "$BARESNAP" ] && command -v file >/dev/null 2>&1; then
    HOST_ARCH=$(uname -m)
    BIN_INFO=$(file -b "$BARESNAP" 2>/dev/null || true)
    case "$HOST_ARCH" in
        x86_64|amd64)
            echo "$BIN_INFO" | grep -qiE "x86-64|80386|Intel 80386" || IS_EMULATED=1
            ;;
        i686|i386|i486|i586)
            echo "$BIN_INFO" | grep -qiE "80386|Intel 80386" || IS_EMULATED=1
            ;;
        aarch64|arm64)
            echo "$BIN_INFO" | grep -qiE "aarch64|ARM aarch64" || IS_EMULATED=1
            ;;
        ppc|ppc64|powerpc|powerpc64|ppc64le)
            echo "$BIN_INFO" | grep -qiE "PowerPC" || IS_EMULATED=1
            ;;
        mips|mips64|mipsel|mips64el)
            echo "$BIN_INFO" | grep -qiE "MIPS" || IS_EMULATED=1
            ;;
        riscv64|riscv32)
            echo "$BIN_INFO" | grep -qiE "RISC-V" || IS_EMULATED=1
            ;;
        s390|s390x)
            echo "$BIN_INFO" | grep -qiE "IBM S/390|s390" || IS_EMULATED=1
            ;;
        *)
            IS_EMULATED=1
            ;;
    esac
fi

# ---------------------------------------------------------------------------
# ulimit size (in KB). Default is 8 MB.
# Can be increased using BRS_TEST_ULIMIT_MB=<megabytes> for native environments
# with low RAM where 8 MB is too restrictive.
#
# Example: BRS_TEST_ULIMIT_MB=16 ./run_tests.sh
# ---------------------------------------------------------------------------
ULIMIT_MB="${BRS_TEST_ULIMIT_MB:-8}"
ULIMIT_KB=$((ULIMIT_MB * 1024))

if [ "$IS_SANITIZED" -eq 1 ] || [ "$IS_EMULATED" -eq 1 ]; then
# -----------------------------------------------------------------------
# Fake RAM guard mode: sanitizers or emulated binary (QEMU). 
#
# ulimit -v cannot be used because:
#   - Sanitizers: require tens of MB of shadow memory upon startup. 
#   - QEMU:       requires memory for its internal threads before
#                 executing a single guest instruction. 
#
# Instead, we use BRS_RAM_GUARD_FAKE_MB to simulate low memory
# at the BareSnap engine level, without altering process limits. 
# -----------------------------------------------------------------------
    if [ "$IS_EMULATED" -eq 1 ]; then
        echo "  [info] Emulated binary detected: using fake RAM guard (no ulimit)"
    else
        echo "  [info] Sanitized binary detected: using fake RAM guard (no ulimit)"
    fi
    set +e
    (
        BRS_RAM_GUARD_FAKE_MB=8 "$BARESNAP" restore "$REPO16" "$SNAP16" "$OUT16" >/dev/null 2>&1
    )
    RESTORE_RC=$?
    set -e
    if [ "$RESTORE_RC" -eq 139 ] || [ "$RESTORE_RC" -eq 137 ]; then
        fail "Engine crashed with simulated low RAM (sanitizer/emulated mode)"
    else
        pass "Engine correctly handled memory restrictions (sanitizer/emulated mode)"
    fi
else
# -----------------------------------------------------------------------
# Native binary: strict ulimit. 
# The size is configurable via BRS_TEST_ULIMIT_MB (default: 8 MB). 
# -----------------------------------------------------------------------
    set +e
    (
        ulimit -v "$ULIMIT_KB" 2>/dev/null
        "$BARESNAP" restore "$REPO16" "$SNAP16" "$OUT16" >/dev/null 2>&1
    )
    RESTORE_RC=$?
    set -e
    if [ "$RESTORE_RC" -eq 139 ] || [ "$RESTORE_RC" -eq 137 ]; then
        fail "Engine crashed due to out of memory (OOM/SegFault)"
    else
        pass "Engine correctly handled memory restrictions (${ULIMIT_MB} MB ulimit)"
    fi
fi
