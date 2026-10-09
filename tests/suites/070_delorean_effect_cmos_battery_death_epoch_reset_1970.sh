#============================================================================
#70. DeLorean Effect: CMOS battery death (Epoch Reset 1970)
#============================================================================
section "70. DeLorean Effect: CMOS battery death (Epoch Reset 1970)"

_t70_san_active() {
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
elif _t70_san_active; then
    log "  [SKIP] 70.1-70.6 skipped under sanitizer build"
    log "  [SKIP] Reason: faketime (LD_PRELOAD) is incompatible with ASan"
    log "  [SKIP] Dynamic ASan: link order error. Static ASan: silent hang."
    log "  [SKIP] Run time tests with release build."
else
    REPO70="$WORK/repo70"
    SRC70="$WORK/src70"
    OUT70="$WORK/out70"
    BASE70="$OUT70/$(basename "$SRC70")"

    rm -rf "$REPO70" "$SRC70" "$OUT70"

    assert_ok "70.1 init" "$BARESNAP" init "$REPO70"

    mkdir -p "$SRC70"
    printf 'delorean test data\n' > "$SRC70/time_file.txt"
    head -c 32768 /dev/urandom > "$SRC70/time_blob.bin"

    assert_ok "70.1 initial create in correct year (2026)" "$BARESNAP" create "$REPO70" "$SRC70"

    SNAP70_1=$(get_latest_snap "$REPO70")

    sleep 1.1
    printf 'delorean v2 post-epoch\n' > "$SRC70/time_file.txt"

    log "🚨 Simulating time reset to 1970 (release build)..."

    set +e
    faketime '1970-01-01 00:00:00' "$BARESNAP" create "$REPO70" "$SRC70" >/dev/null 2>&1
    RC70_CREATE=$?
    set -e

    if [ "$RC70_CREATE" -eq 0 ]; then
        pass "70.2 incremental create under faketime 1970"

        SNAPS70=$(get_snap_count "$REPO70")
        assert_eq "70.3 zero collisions by UUID v4 (2 snapshots)" "2" "$SNAPS70"

        assert_ok "70.4 verify without crash after time eclipse" "$BARESNAP" verify "$REPO70"
    else
        fail "70.2 incremental create under faketime 1970 failed (rc=$RC70_CREATE)"
        fail "70.3 skipped because create failed"
        fail "70.4 skipped because create failed"
    fi

    if [ -d "$REPO70" ] && [ "$RC70_CREATE" -eq 0 ]; then
        assert_ok "70.5 prune --keep-last 1 under temporal distortion" "$BARESNAP" prune "$REPO70" --keep-last 1

        SNAPS70_AFTER=$(get_snap_count "$REPO70")
        assert_eq "70.5 prune kept 1 snapshot" "1" "$SNAPS70_AFTER"

        SNAP70_LAST=$(get_latest_snap "$REPO70")

        rm -rf "$OUT70"
        assert_ok "70.6 restore post-temporal chaos" "$BARESNAP" restore "$REPO70" "$SNAP70_LAST" "$OUT70"
        assert_ok "70.6 global verify positive after temporal chaos" "$BARESNAP" verify "$REPO70"
    else
        log "  [SKIP] 70.5 prune skipped (no successful 1970 create)"
        log "  [SKIP] 70.6 restore/verify skipped (no successful 1970 create)"
    fi
fi
