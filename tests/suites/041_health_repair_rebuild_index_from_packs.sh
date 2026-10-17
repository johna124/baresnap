section "41. Health repair: rebuild index from packs"

if ! "$BARESNAP" health --help >/dev/null 2>&1; then
    log "  [SKIP] health command not implemented"
else
    REPO="$WORK/repo41"
    SRC="$WORK/src41"
    OUT="$WORK/out41"
    BASE="$OUT/$(basename "$SRC")"

    assert_ok "init" "$BARESNAP" init "$REPO"
    mkdir -p "$SRC"
    printf 'rebuild index test\n' > "$SRC/rebuild.txt"
    head -c 32768 /dev/urandom > "$SRC/blob.bin"
    assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"

    rm -f "$REPO"/index/*.idx
    rm -f "$REPO"/index/*.blm

    set +e
    "$BARESNAP" verify "$REPO" >/dev/null 2>&1
    VERIFY_RC=$?
    set -e

    if [ "$VERIFY_RC" -ne 0 ]; then
        pass "verify fails after deleting index"
    else
        fail "verify should fail without index"
    fi

    # Health repair: allow normal failure but NOT crash
    env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" --repair </dev/null >/dev/null 2>&1
    REPAIR_RC41=$?

    if [ "$REPAIR_RC41" -ge 128 ]; then
        fail "41.health repair CRASHED with signal $((REPAIR_RC41 - 128))"
    elif [ "$REPAIR_RC41" -eq 124 ]; then
        fail "41.health repair timed out"
    else
        pass "41.health repair completed without crash (rc=$REPAIR_RC41)"
    fi

    assert_ok "verify passes after repair (index rebuilt)" "$BARESNAP" verify "$REPO"

    SNAP=$(get_latest_snap "$REPO")
    assert_ok "restore after repair" "$BARESNAP" restore "$REPO" "$SNAP" "$OUT"
    assert_ok "rebuild.txt correct" cmp -s "$SRC/rebuild.txt" "$BASE/rebuild.txt"
    assert_ok "blob.bin correct" cmp -s "$SRC/blob.bin" "$BASE/blob.bin"
fi
