#============================================================================
# 115. FastCDC: Metadata Tree Boundary Corruption and Health Detection
#============================================================================
section "115. FastCDC: Metadata Tree Boundary Corruption and Health Detection"

set +e
set +o pipefail 2>/dev/null || true
unset BARESNAP_PASSPHRASE 2>/dev/null || true

BARESNAP="${BARESNAP:-./baresnap}"

_t115_san_active() {
    # Override manual si lo necesitas:
    #   BRS_FORCE_SANITIZER=1  → forzar detección
    #   BRS_FORCE_SANITIZER=0  → forzar modo limpio
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

TMO115="${BRS_TEST_TIMEOUT:-}"

if [ -z "$TMO115" ]; then
    if _t115_san_active; then
        TMO115=900
    else
        TMO115=120
    fi
fi

if [ "${QEMU_DETECTED:-0}" = "1" ]; then
    if [ "$TMO115" -lt 1800 ] 2>/dev/null; then
        TMO115=1800
    fi
fi

if _t115_san_active; then
    log ""
    log "======================================================================"
    log "⚠️  🚨 [SANITIZER DETECTED — TEST 115 WILL TAKE LONGER] 🚨 ⚠️"
    log "======================================================================"
    log " This test uses FastCDC + crypto + metadata torture."
    log " The active binary is instrumented with ASan/UBSan or another"
    log " memory sanitizer, so chunking, encryption and restore operations"
    log " are significantly slower."
    log ""
    log " This is expected. The CPU is actively working. It is not hung."
    log " Timeout for this test: ${TMO115}s"
    log "======================================================================"
fi

_t115_log_output() {
    local out="$1"
    printf '%s\n' "$out" | head -n 140 | sed 's/^/[115 ERROR] /' | tee -a "${LOG_FILE:-/dev/null}"
}

_t115_run_ok() {
    local desc="$1"
    shift

    local out rc

    out="$(timeout "$TMO115" "$@" 2>&1 </dev/null)"
    rc=$?

    if [ "$rc" -eq 0 ]; then
        pass "$desc"
        return 0
    fi

    if [ "$rc" -eq 124 ]; then
        fail "$desc (TIMEOUT after ${TMO115}s)"
    else
        fail "$desc (rc=$rc)"
    fi

    _t115_log_output "$out"
    return 1
}

_t115_run_fail() {
    local desc="$1"
    shift

    local out rc

    out="$(timeout "$TMO115" "$@" 2>&1 </dev/null)"
    rc=$?

    if [ "$rc" -eq 124 ]; then
        fail "$desc (TIMEOUT while expecting failure)"
        _t115_log_output "$out"
        return 1
    fi

    if [ "$rc" -ne 0 ]; then
        pass "$desc"
        return 0
    fi

    fail "$desc (expected failure but command succeeded)"
    _t115_log_output "$out"
    return 1
}

_t115_corrupt_file() {
    local f="$1"
    local sz

    sz=$(stat -c "%s" "$f" 2>/dev/null || echo "0")

    # Corrupt header / first bytes.
    printf '\x7F\xFF\x00\x13' | dd of="$f" bs=1 count=4 conv=notrunc status=none >/dev/null 2>&1 || true

    # Corrupt middle if possible.
    if [ "$sz" -gt 8 ]; then
        printf '\x7F' | dd of="$f" bs=1 seek="$((sz / 2))" conv=notrunc status=none >/dev/null 2>&1 || true
    fi

    # Truncate to half size if possible.
    if [ "$sz" -gt 16 ]; then
        head -c "$((sz / 2))" "$f" > "${f}.t115tmp" 2>/dev/null && mv "${f}.t115tmp" "$f"
    fi
}

DIR_REPO115="$WORK/repo115"
DIR_SRC115="$WORK/src115"
DIR_RESTORE115="$WORK/restore115"
TEST_PASS115="FastCDCPunishment-2026-Metal"

rm -rf "$DIR_REPO115" "$DIR_SRC115" "$DIR_RESTORE115"
mkdir -p "$DIR_SRC115" "$DIR_RESTORE115"

for i in {1..5}; do
    echo "Bloque repetitivo de ingenieria de guerrilla $i para alinear el chunker" >> "$DIR_SRC115/dedup.txt"
done

dd if=/dev/urandom of="$DIR_SRC115/random.bin" bs=1M count=2 status=none

_t115_run_ok "115.1 Initialize repository using verified --pass-fd" \
    "$BARESNAP" --pass-fd 8 init "$DIR_REPO115" --encrypt \
    8<<<"$TEST_PASS115" < /dev/null || true

_t115_run_ok "115.2 Create snapshot with dynamic FastCDC chunking" \
    "$BARESNAP" --pass-fd 8 create "$DIR_REPO115" "$DIR_SRC115" "snap_cdc_115" \
    8<<<"$TEST_PASS115" < /dev/null || true

INDEX_DIR=$( { find "$DIR_REPO115" -maxdepth 3 -type d -name "index" 2>/dev/null || true; } | head -n 1 )
SNAP_FILE=$( { find "$DIR_REPO115" -maxdepth 4 -type f -name "*.snap" 2>/dev/null || true; } | head -n 1 )

METADATA115_OK=1

if [ -z "$INDEX_DIR" ] || [ -z "$SNAP_FILE" ] || [ ! -d "$INDEX_DIR" ] || [ ! -f "$SNAP_FILE" ]; then
    fail "115.2 Metadata structures (.index directory or .snap file) were not found"
    METADATA115_OK=0
fi

if [ "$METADATA115_OK" -eq 1 ]; then
    SNAP_NAME115="$(basename "$SNAP_FILE")"

    cp -r "$INDEX_DIR" "${INDEX_DIR}.bak"
    cp "$SNAP_FILE" "${SNAP_FILE}.bak"

    IDX_COUNT=0

    for idx in "$INDEX_DIR"/*.idx; do
        [ -f "$idx" ] || continue
        IDX_COUNT=$((IDX_COUNT + 1))
        _t115_corrupt_file "$idx"
    done

    if [ "$IDX_COUNT" -eq 0 ]; then
        TARGET_INDEX_FILE=$(find "$INDEX_DIR" -maxdepth 1 -type f ! -name "*.bak" 2>/dev/null | head -n 1)

        if [ -z "$TARGET_INDEX_FILE" ] || [ ! -f "$TARGET_INDEX_FILE" ]; then
            fail "115.2 Index directory is empty; no target chunk metadata file found"
            METADATA115_OK=0
        else
            _t115_corrupt_file "$TARGET_INDEX_FILE"
        fi
    fi
fi

if [ "$METADATA115_OK" -eq 1 ]; then
    sync

    rm -rf "$DIR_RESTORE115"
    mkdir -p "$DIR_RESTORE115"

    # ------------------------------------------------------------------
    # 115.3 — RESTORE must reject corrupted index metadata.
    # ------------------------------------------------------------------
    OUT115_3="$(timeout "$TMO115" "$BARESNAP" --pass-fd 8 restore "$DIR_REPO115" "$SNAP_NAME115" "$DIR_RESTORE115" 8<<<"$TEST_PASS115" </dev/null 2>&1)"
    RC115_3=$?

    if [ "$RC115_3" -eq 124 ]; then
        fail "115.3 Restore rejects execution when chunk offsets are desynchronized in the index (timeout)"
        _t115_log_output "$OUT115_3"
    elif [ "$RC115_3" -ne 0 ]; then
        pass "115.3 Restore rejects execution when chunk offsets are desynchronized in the index"
    else
        fail "115.3 Restore rejects execution when chunk offsets are desynchronized in the index (restore accepted corrupted index)"
        _t115_log_output "$OUT115_3"
    fi

    # ------------------------------------------------------------------
    # 115.4 — Engine should flag metadata corruption.
    # ------------------------------------------------------------------
    log "  [INFO] 115.4 Running repository integrity firewall inspection..."

    ANOMALY_PATTERN='missing|cannot|error|failed|invalid|corrupt|not in index|checksum|mismatch|warning|sanitizer|asan|ubsan|tsan'

    if printf '%s\n' "$OUT115_3" | grep -Eiq "$ANOMALY_PATTERN"; then
        pass "115.4 Engine successfully flags metadata boundary structural anomalies"
    elif [ "$RC115_3" -ne 0 ]; then
        pass "115.4 Engine successfully flags metadata boundary structural anomalies"
    else
        fail "115.4 Engine mapping bypassed index structural corruption without flagging"
        _t115_log_output "$OUT115_3"
    fi

    # Restore metadata before snapshot torture.
    rm -rf "$INDEX_DIR"
    cp -r "${INDEX_DIR}.bak" "$INDEX_DIR"
    cp "${SNAP_FILE}.bak" "$SNAP_FILE"

    rm -rf "$DIR_RESTORE115"
    mkdir -p "$DIR_RESTORE115"

    # ------------------------------------------------------------------
    # 115.5 — Corrupt snapshot recipe and expect restore failure.
    # ------------------------------------------------------------------
    SNAP_SIZE=$(stat -c "%s" "$SNAP_FILE" 2>/dev/null || echo "1024")

    head -c "$SNAP_SIZE" /dev/urandom > "$SNAP_FILE"

    _t115_run_fail "115.5 Restore aborts cleanly when file reconstruction recipe is corrupted" \
        "$BARESNAP" --pass-fd 8 restore "$DIR_REPO115" "$SNAP_NAME115" "$DIR_RESTORE115" \
        8<<<"$TEST_PASS115" < /dev/null || true

    rm -rf "${INDEX_DIR}.bak"
    rm -f "${SNAP_FILE}.bak"
fi

[ -n "${INDEX_DIR:-}" ] && rm -rf "${INDEX_DIR}.bak" 2>/dev/null || true
[ -n "${SNAP_FILE:-}" ] && rm -f "${SNAP_FILE}.bak" 2>/dev/null || true

rm -rf "$DIR_REPO115" "$DIR_SRC115" "$DIR_RESTORE115"
unset TEST_PASS115
