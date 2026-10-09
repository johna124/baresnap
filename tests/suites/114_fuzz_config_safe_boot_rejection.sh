#============================================================================
# 114. Fuzzing: Repository Configuration Safe Boot Rejection
#============================================================================
section "114. Fuzzing: Repository Configuration Safe Boot Rejection"

set +e
set +o pipefail 2>/dev/null || true
unset BARESNAP_PASSPHRASE 2>/dev/null || true

BARESNAP="${BARESNAP:-./baresnap}"

_t114_san_detected() {
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

TMO114="${BRS_TEST_TIMEOUT:-}"

if [ -z "$TMO114" ]; then
    if _t114_san_detected; then
        TMO114=600
    else
        TMO114=60
    fi
fi

if [ "${QEMU_DETECTED:-0}" = "1" ]; then
    if [ "$TMO114" -lt 1200 ] 2>/dev/null; then
        TMO114=1200
    fi
fi

_t114_banner() {
    if _t114_san_detected; then
        log ""
        log "======================================================================"
        log "⚠️  🚨 [SANITIZER DETECTED — TEST 114 WILL TAKE LONGER] 🚨 ⚠️"
        log "======================================================================"
        log " The active binary is instrumented with ASan/UBSan or another"
        log " memory sanitizer. Repository validation and cryptographic checks"
        log " are significantly slower in this mode."
        log ""
        log " This is expected. The test is not hung."
        log " Timeout for this test: ${TMO114}s"
        log "======================================================================"
    fi
}

_t114_banner

_t114_run_ok() {
    local desc="$1"
    shift

    local logfile="${LOG_FILE:-/dev/null}"
    local out rc

    out="$(timeout "$TMO114" "$@" 2>&1 </dev/null)"
    rc=$?

    if [ "$rc" -eq 0 ]; then
        pass "$desc"
        return 0
    fi

    if [ "$rc" -eq 124 ]; then
        fail "$desc (TIMEOUT after ${TMO114}s)"
    else
        fail "$desc (rc=$rc)"
    fi

    printf '%s\n' "$out" | head -n 100 | sed 's/^/[114 ERROR] /' | tee -a "$logfile"

    return 1
}

_t114_run_fail() {
    local desc="$1"
    shift

    local logfile="${LOG_FILE:-/dev/null}"
    local out rc

    out="$(timeout "$TMO114" "$@" 2>&1 </dev/null)"
    rc=$?

    if [ "$rc" -eq 124 ]; then
        fail "$desc (TIMEOUT while expecting failure)"
        printf '%s\n' "$out" | head -n 100 | sed 's/^/[114 ERROR] /' | tee -a "$logfile"
        return 1
    fi

    if [ "$rc" -ne 0 ]; then
        pass "$desc"
        return 0
    fi

    fail "$desc (expected failure but command succeeded)"
    printf '%s\n' "$out" | head -n 100 | sed 's/^/[114 ERROR] /' | tee -a "$logfile"

    return 1
}

DIR_REPO114="$WORK/repo114"
DIR_SRC114="$WORK/src114"
TEST_PASS114="FuzzPass-2026-MetalPunishment"

rm -rf "$DIR_REPO114" "$DIR_SRC114"
mkdir -p "$DIR_SRC114"

echo "Datos de calibracion para el fuzzing v2.4.1" > "$DIR_SRC114/file.txt"

INIT114_OK=1

_t114_run_ok "114.1 Initialize clean repository for fuzzing" \
    "$BARESNAP" --pass-fd 5 init "$DIR_REPO114" --encrypt \
    5<<<"$TEST_PASS114" < /dev/null || INIT114_OK=0

if [ "$INIT114_OK" -ne 1 ]; then
    fail "114.2 Skipped: repository initialization failed"
    fail "114.3 Skipped: repository initialization failed"
    fail "114.4 Skipped: repository initialization failed"
else
    CONFIG_FILE114="$DIR_REPO114/.config"

    if [ ! -f "$CONFIG_FILE114" ]; then
        CONFIG_FILE114=$(find "$DIR_REPO114" -maxdepth 3 \( -name ".config" -o -name "config" \) -type f 2>/dev/null | head -n 1)
    fi

    if [ -z "$CONFIG_FILE114" ] || [ ! -f "$CONFIG_FILE114" ]; then
        fail "114.1 Config file not found after init"
        fail "114.2 Skipped: config file not found"
        fail "114.3 Skipped: config file not found"
        fail "114.4 Skipped: config file not found"
    else
        CONFIG_SIZE114=$(stat -c "%s" "$CONFIG_FILE114" 2>/dev/null || echo "0")
        cp "$CONFIG_FILE114" "${CONFIG_FILE114}.bak"

        # --- TORTURE 1 ---
        printf '\x00' | dd of="$CONFIG_FILE114" bs=1 count=1 conv=notrunc status=none >/dev/null 2>&1

        _t114_run_fail "114.2 Create fails cleanly when config Magic Header is corrupted" \
            "$BARESNAP" --pass-fd 5 create "$DIR_REPO114" "$DIR_SRC114" "fuzz-1" \
            5<<<"$TEST_PASS114" < /dev/null || true

        # --- TORTURE 2 ---
        cp "${CONFIG_FILE114}.bak" "$CONFIG_FILE114"

        if [ "$CONFIG_SIZE114" -gt 0 ]; then
            dd if=/dev/urandom of="$CONFIG_FILE114" bs="$CONFIG_SIZE114" count=1 conv=notrunc status=none >/dev/null 2>&1
        fi

        log "  [INFO] 114.3 Injecting total binary entropy into .config..."

        OUT114_3="$(timeout "$TMO114" "$BARESNAP" --pass-fd 5 list "$DIR_REPO114" 5<<<"$TEST_PASS114" </dev/null 2>&1)"
        RC114_3=$?

        if [ "$RC114_3" -eq 124 ]; then
            fail "114.3 List handles total binary garbage safely without crash (timeout)"
            printf '%s\n' "$OUT114_3" | head -n 100 | sed 's/^/[114 ERROR] /' | tee -a "${LOG_FILE:-/dev/null}"
        elif [ "$RC114_3" -ge 128 ]; then
            fail "114.3 List crashed violently (rc=$RC114_3)"
            printf '%s\n' "$OUT114_3" | head -n 100 | sed 's/^/[114 ERROR] /' | tee -a "${LOG_FILE:-/dev/null}"
        else
            pass "114.3 List handles total binary garbage safely without crash"
        fi

        # --- TORTURE 3 ---
        cp "${CONFIG_FILE114}.bak" "$CONFIG_FILE114"

        if [ "$CONFIG_SIZE114" -gt 2 ]; then
            OFFSET_SABOTAJE114=$((1 + RANDOM % (CONFIG_SIZE114 - 2)))
        else
            OFFSET_SABOTAJE114=1
        fi

        printf '\xFF' | dd of="$CONFIG_FILE114" bs=1 seek="$OFFSET_SABOTAJE114" conv=notrunc status=none >/dev/null 2>&1

        _t114_run_fail "114.4 Verify rejects repository startup when cipher Tag is mutated" \
            "$BARESNAP" --pass-fd 5 verify "$DIR_REPO114" \
            5<<<"$TEST_PASS114" < /dev/null || true

        rm -f "${CONFIG_FILE114}.bak"
    fi
fi

rm -rf "$DIR_REPO114" "$DIR_SRC114"
unset TEST_PASS114
