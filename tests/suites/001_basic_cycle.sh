# ============================================================
# 01. Basic cycle
# ============================================================
section "01. Basic cycle: init / create / list / restore"
REPO="$WORK/repo01"
SRC="$WORK/src01"
OUT="$WORK/out01"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO" 

mkdir -p "$SRC/subdir"
printf 'hello world
' > "$SRC/file.txt"
printf 'nested file
' > "$SRC/subdir/nested.txt"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
SNAP_COUNT=$(get_snap_count "$REPO")
assert_eq "list shows 1 snapshot" "1" "$SNAP_COUNT"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
assert_ok "identical content (file.txt)" cmp -s "$SRC/file.txt" "$BASE/file.txt"
assert_ok "identical content (nested.txt)" cmp -s "$SRC/subdir/nested.txt" "$BASE/subdir/nested.txt"

