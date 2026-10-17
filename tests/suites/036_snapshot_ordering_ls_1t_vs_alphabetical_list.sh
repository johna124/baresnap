# ============================================================
# 36. Snapshot ordering: ls -1t vs alphabetical list
# ============================================================
section "36. Snapshot ordering: ls -1t vs alphabetical list"
REPO="$WORK/repo36"
SRC="$WORK/src36"
OUT="$WORK/out36"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'ORDER TEST v1
' > "$SRC/order.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'ORDER TEST v2
' > "$SRC/order.txt"
assert_ok "create snap 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP_OLDEST=$(ls -1t "$REPO/snapshots/"*.snap 2>/dev/null | tail -1 | xargs basename)
SNAP_NEWEST=$(ls -1t "$REPO/snapshots/"*.snap 2>/dev/null | head -1 | xargs basename)
rm -rf "$OUT"
assert_ok "restore oldest snapshot" "$BARESNAP" restore "$REPO" "$SNAP_OLDEST" "$OUT"
RESTORED=$(cat "$BASE/order.txt" 2>/dev/null)
assert_eq "old snapshot has v1" "ORDER TEST v1" "$RESTORED"
rm -rf "$OUT"
assert_ok "restore newest snapshot" "$BARESNAP" restore "$REPO" "$SNAP_NEWEST" "$OUT"
RESTORED=$(cat "$BASE/order.txt" 2>/dev/null)
assert_eq "new snapshot has v2" "ORDER TEST v2" "$RESTORED"

