# ============================================================
# 40. Health repair: moves corrupt snapshots to damaged/
# ============================================================
section "40. Health repair: corrupt snapshots to damaged/"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
REPO="$WORK/repo40"
SRC="$WORK/src40"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'repair test v1
' > "$SRC/file.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'repair test v2
' > "$SRC/file.txt"
assert_ok "create snap 2" "$BARESNAP" create "$REPO" "$SRC"
SNAPS_BEFORE=$(get_snap_count "$REPO")
assert_eq "2 snapshots before corrupting" "2" "$SNAPS_BEFORE"
rm -f "$REPO/packs/"*.pack
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" --repair </dev/null >/dev/null 2>&1 || true
DAMAGED_COUNT=$(ls "$REPO/damaged/"*.snap 2>/dev/null | wc -l)
if [ "$DAMAGED_COUNT" -ge 1 ]; then
pass "repair moved corrupt snapshots to damaged/ ($DAMAGED_COUNT)"
else
fail "repair did not move snapshots to damaged/"
fi
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "snapshots/ empty after repair" "0" "$SNAPS_AFTER"
fi

