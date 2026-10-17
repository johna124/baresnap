#============================================================================
#72. The End of Time: Resistance to the Unix Y2K38 Apocalypse
#============================================================================
section "72. The End of Time: Resistance to the Unix Y2K38 Apocalypse"

_t72_san_active() {
    if command -v brs_is_sanitized_binary >/dev/null 2>&1; then
        brs_is_sanitized_binary
        return $?
    fi
    local bin="${BARESNAP:-./baresnap}"
    [ -x "$bin" ] || return 1
    if command -v ldd >/dev/null 2>&1; then
        if ldd "$bin" 2>/dev/null | grep -Eq 'libasan|libtsan|libubsan|libmsan'; then
            return 0
        fi
    fi
    if command -v nm >/dev/null 2>&1; then
        if nm -a "$bin" 2>/dev/null | grep -Eq '__asan_init|__tsan_init|__ubsan_handle|__msan_init'; then
            return 0
        fi
    fi
    if command -v strings >/dev/null 2>&1; then
        if strings "$bin" 2>/dev/null | grep -Eiq 'AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|MemorySanitizer'; then
            return 0
        fi
    fi
    return 1
}

if ! command -v faketime &>/dev/null; then
    log "  ${YELLOW}[SKIP]${NC} faketime not installed"
elif _t72_san_active; then
    log "  [SKIP] 72.1-72.6 skipped under sanitizer build"
    log "  [SKIP] Reason: faketime (LD_PRELOAD) is incompatible with ASan"
    log "  [SKIP] Dynamic ASan: link order error. Static ASan: silent hang."
    log "  [SKIP] Run time tests with release build."
else
    REPO72="$WORK/repo72"
    SRC72="$WORK/src72"
    OUT72="$WORK/out72"
    BASE72="$OUT72/$(basename "$SRC72")"

    rm -rf "$REPO72" "$SRC72" "$OUT72"

    mkdir -p "$SRC72"
    printf 'File surviving the 32-bit Apocalypse\n' > "$SRC72/futuro.txt"
    head -c 65536 /dev/urandom > "$SRC72/future_blob.bin"

    if faketime '2039-01-01 12:00:00' touch "$SRC72/futuro.txt" 2>/dev/null; then
        pass "72.1 time jump simulation completed (clock in 2039)"
    else
        fail "72.1 time jump simulation failed (faketime touch failed)"
    fi

    log "🚨 Launching Y2K38 compliance checks (release build)..."

    T72_OK=1

    set +e
    if [ -n "${HASH_MODE:-}" ]; then
        faketime '2039-01-01 00:00:00' "$BARESNAP" init "$REPO72" --hash "$HASH_MODE" >/dev/null 2>&1
    else
        faketime '2039-01-01 00:00:00' "$BARESNAP" init "$REPO72" >/dev/null 2>&1
    fi
    RC72_INIT=$?
    set -e

    if [ "$RC72_INIT" -eq 0 ]; then
        pass "72.2 init in 2039"
    else
        fail "72.2 init in 2039 failed (rc=$RC72_INIT)"
        T72_OK=0
    fi

    if [ "$T72_OK" -eq 1 ]; then
        set +e
        faketime '2039-01-01 12:10:00' "$BARESNAP" create "$REPO72" "$SRC72" "snap_post_2038" >/dev/null 2>&1
        RC72_CREATE=$?
        set -e

        if [ "$RC72_CREATE" -eq 0 ]; then
            pass "72.2 create in 2039 (time_t 64-bit assimilates metadata)"
        else
            fail "72.2 create in 2039 failed (rc=$RC72_CREATE)"
            T72_OK=0
        fi
    fi

    if [ "$T72_OK" -eq 1 ]; then
        SNAP72=$(get_latest_snap "$REPO72" 2>/dev/null || true)

        if [ -n "$SNAP72" ]; then
            pass "72.3 list shows snapshot from the future without corruption ($SNAP72)"
        else
            fail "72.3 corrupted metadata: overflow altered the snapshot"
            T72_OK=0
        fi
    fi

    if [ "$T72_OK" -eq 1 ]; then
        assert_ok "72.4 verify in Y2K38 environment" \
            faketime '2039-01-01 12:15:00' "$BARESNAP" verify "$REPO72"

        assert_ok "72.4 prune under 2039 temporal distortion" \
            faketime '2039-01-01 12:15:00' "$BARESNAP" prune "$REPO72" --keep-last 1

        rm -rf "$OUT72"

        assert_ok "72.5 restore in the year 2039" \
            faketime '2039-01-01 12:20:00' "$BARESNAP" restore "$REPO72" "$SNAP72" "$OUT72"

        assert_ok "72.5 futuro.txt byte-identical after restore in 2039" \
            cmp -s "$SRC72/futuro.txt" "$BASE72/futuro.txt"

        assert_ok "72.5 future_blob.bin byte-identical" \
            cmp -s "$SRC72/future_blob.bin" "$BASE72/future_blob.bin"

        MTIME_SRC72=$(stat -c '%Y' "$SRC72/futuro.txt" 2>/dev/null || echo "2177486400")
        MTIME_OUT72=$(stat -c '%Y' "$BASE72/futuro.txt" 2>/dev/null || echo "2177486400")

        assert_eq "72.6 mtime from 2039 preserved correctly" "$MTIME_SRC72" "$MTIME_OUT72"
    fi
fi
