# ============================================================
# 04. Real hardlinks
# ============================================================
section "04. Real hardlinks"
REPO="$WORK/repo04"
SRC="$WORK/src04"
OUT="$WORK/out04"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'shared content
' > "$SRC/original.txt"
ln "$SRC/original.txt" "$SRC/hardlink.txt"
INO_SRC1=$(stat -c '%i' "$SRC/original.txt")
INO_SRC2=$(stat -c '%i' "$SRC/hardlink.txt")
assert_eq "source: same inode" "$INO_SRC1" "$INO_SRC2"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
INO_OUT1=$(stat -c '%i' "$BASE/original.txt")
INO_OUT2=$(stat -c '%i' "$BASE/hardlink.txt")
assert_eq "restore: same inode" "$INO_OUT1" "$INO_OUT2"
assert_ok "correct content" test "$(cat "$BASE/original.txt")" = "shared content"
rm "$BASE/original.txt"
assert_ok "hardlink survives deleting original" test "$(cat "$BASE/hardlink.txt")" = "shared content"

