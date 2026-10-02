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
# Detect if the binary is instrumented with ASan/TSan/UBSan.
# Sanitizers need tens of MB of shadow memory at startup,
# so ulimit -v 8192 hangs them before executing an instruction.
IS_SANITIZED=0
if ldd "$BARESNAP" 2>/dev/null | grep -qE "libasan|libtsan|libubsan"; then
IS_SANITIZED=1
elif nm "$BARESNAP" 2>/dev/null | grep -qE "__asan_init|__tsan_init|__ubsan"; then
IS_SANITIZED=1
fi
if [ "$IS_SANITIZED" -eq 1 ]; then
# Under sanitizers: use BRS_RAM_GUARD_FAKE_MB to simulate memory
# low at the engine level, without ulimit (which would hang the ASan runtime).
# We verify that the engine does not crash and handles the restriction.
set +e
(
BRS_RAM_GUARD_FAKE_MB=8 "$BARESNAP" restore "$REPO16" "$SNAP16" "$OUT16" >/dev/null 2>&1
)
RESTORE_RC=$?
set -e
if [ "$RESTORE_RC" -eq 139 ] || [ "$RESTORE_RC" -eq 137 ]; then
fail "Engine crashed under sanitizers with simulated low RAM"
else
pass "Engine correctly handled memory restrictions (sanitizer mode)"
fi
else
# Normal binary: strict 8 MB ulimit as always.
(
ulimit -v 8192 2>/dev/null
"$BARESNAP" restore "$REPO16" "$SNAP16" "$OUT16" >/dev/null 2>&1
)
RESTORE_RC=$?
if [ "$RESTORE_RC" -eq 139 ] || [ "$RESTORE_RC" -eq 137 ]; then
fail "Engine crashed due to out of memory (OOM/SegFault)"
else
pass "Engine correctly handled memory restrictions"
fi
fi

