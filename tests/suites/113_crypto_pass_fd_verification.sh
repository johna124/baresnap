#============================================================================
# 113. Crypto: Passphrase Injection via File Descriptor Verification
#============================================================================
section "113. Crypto: Passphrase Injection via File Descriptor Verification"

set +e
set +o pipefail 2>/dev/null || true
unset BARESNAP_PASSPHRASE 2>/dev/null || true

BARESNAP="${BARESNAP:-./baresnap}"

_t113_san_detected() {
    # Override manual:
    #   BRS_FORCE_SANITIZER=1 -> forzar banner/timeout largo
    #   BRS_FORCE_SANITIZER=0 -> forzar modo limpio
    if [ "${BRS_FORCE_SANITIZER:-}" = "1" ]; then
        return 0
    fi

    if [ "${BRS_FORCE_SANITIZER:-}" = "0" ]; then
        return 1
    fi

    local bin="${BARESNAP:-./baresnap}"

    [ -x "$bin" ] || return 1

    if command -v ldd >/dev/null 2>&1; then
        if ldd "$bin" 2>/dev/null | grep -Eq 'libasan|libubsan|libtsan|liblsan|libmsan'; then
            return 0
        fi
    fi

    if command -v nm >/dev/null 2>&1; then
        if nm -a "$bin" 2>/dev/null | grep -Eq '__asan_init|__ubsan_handle|__tsan_init|__msan_init'; then
            return 0
        fi
    fi

    if command -v strings >/dev/null 2>&1; then
        if strings "$bin" 2>/dev/null | grep -Eiq 'AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|MemorySanitizer'; then
            return 0
        fi
    fi

    return 1
}

TMO113="${BRS_TEST_TIMEOUT:-}"

if [ -z "$TMO113" ]; then
    if _t113_san_detected; then
        TMO113=600
    else
        TMO113=60
    fi
fi

if [ "${QEMU_DETECTED:-0}" = "1" ]; then
    if [ "$TMO113" -lt 1200 ] 2>/dev/null; then
        TMO113=1200
    fi
fi

_t113_banner() {
    if _t113_san_detected; then
        log ""
        log "======================================================================"
        log "⚠️  🚨 [SANITIZER DETECTED — TEST 113 WILL TAKE LONGER] 🚨 ⚠️"
        log "======================================================================"
        log " The active binary is instrumented with ASan/UBSan or another"
        log " memory sanitizer. Cryptographic key derivation and restore paths"
        log " are significantly slower in this mode."
        log ""
        log " This is expected. The test is not hung."
        log " Timeout for this test: ${TMO113}s"
        log "======================================================================"
    fi
}

_t113_banner

_t113_run_ok() {
    local desc="$1"
    shift

    local logfile="${LOG_FILE:-/dev/null}"
    local out rc

    out="$(timeout "$TMO113" "$@" 2>&1 </dev/null)"
    rc=$?

    if [ "$rc" -eq 0 ]; then
        pass "$desc"
        return 0
    fi

    if [ "$rc" -eq 124 ]; then
        fail "$desc (TIMEOUT after ${TMO113}s)"
    else
        fail "$desc (rc=$rc)"
    fi

    printf '%s\n' "$out" | head -n 100 | sed 's/^/[113 ERROR] /' | tee -a "$logfile"

    return 1
}

_t113_run_fail() {
    local desc="$1"
    shift

    local logfile="${LOG_FILE:-/dev/null}"
    local out rc

    out="$(timeout "$TMO113" "$@" 2>&1 </dev/null)"
    rc=$?

    if [ "$rc" -eq 124 ]; then
        fail "$desc (TIMEOUT while expecting failure)"
        printf '%s\n' "$out" | head -n 100 | sed 's/^/[113 ERROR] /' | tee -a "$logfile"
        return 1
    fi

    if [ "$rc" -ne 0 ]; then
        pass "$desc"
        return 0
    fi

    fail "$desc (expected failure but command succeeded)"
    printf '%s\n' "$out" | head -n 100 | sed 's/^/[113 ERROR] /' | tee -a "$logfile"

    return 1
}

DIR_REPO113="$WORK/repo113"
DIR_SRC113="$WORK/src113"
DIR_RESTORE113="$WORK/restore113"
TEST_PASS113="SpartanPass-2026-MetalReal-XChaCha"

rm -rf "$DIR_REPO113" "$DIR_SRC113" "$DIR_RESTORE113"
mkdir -p "$DIR_SRC113" "$DIR_RESTORE113"

echo "Metal real e ingenieria de guerrilla sin burocracia v2.4.1" > "$DIR_SRC113/data1.txt"

_t113_run_ok "113.1 Initialize repository using --pass-fd 5" \
    "$BARESNAP" --pass-fd 5 init "$DIR_REPO113" --encrypt \
    5<<<"$TEST_PASS113" < /dev/null || true

_t113_run_ok "113.2 Create backup snapshot with --pass-fd 5" \
    "$BARESNAP" --pass-fd 5 create "$DIR_REPO113" "$DIR_SRC113" "snap_fd_113" \
    5<<<"$TEST_PASS113" < /dev/null || true

_t113_run_fail "113.3 Verify operation rejects corrupted passphrase cleanly" \
    "$BARESNAP" --pass-fd 5 verify "$DIR_REPO113" \
    5<<<"ClaveFalsa123" < /dev/null || true

SNAP_NAME113=$(ls -1 "$DIR_REPO113/snapshots" 2>/dev/null | grep '\.snap$' | head -n 1)

if [ -z "$SNAP_NAME113" ]; then
    fail "113.4 Restore snapshot using --pass-fd 5 (no snapshot found)"
else
    _t113_run_ok "113.4 Restore snapshot using --pass-fd 5" \
        "$BARESNAP" --pass-fd 5 restore "$DIR_REPO113" "$SNAP_NAME113" "$DIR_RESTORE113" \
        5<<<"$TEST_PASS113" < /dev/null || true
fi

log "  [INFO] Executing cross-binary integrity diff..."

if [ -d "$DIR_RESTORE113/src113" ]; then
    if diff -r "$DIR_SRC113" "$DIR_RESTORE113/src113" >/dev/null 2>&1; then
        pass "113.5 Payload cross-diff matches perfectly bit by bit"
    else
        fail "113.5 Payload cross-diff detected binary corruption"
    fi
else
    if diff -r "$DIR_SRC113" "$DIR_RESTORE113" >/dev/null 2>&1; then
        pass "113.5 Payload cross-diff matches perfectly bit by bit"
    else
        fail "113.5 Payload cross-diff detected binary corruption"
    fi
fi

rm -rf "$DIR_REPO113" "$DIR_SRC113" "$DIR_RESTORE113"
unset TEST_PASS113
