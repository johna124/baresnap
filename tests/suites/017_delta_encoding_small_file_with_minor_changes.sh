# ============================================================
# 17. Delta Encoding: small file with minor changes
# ============================================================
section "17. Delta Encoding: small file with minor changes"
REPO="$WORK/repo17"
SRC="$WORK/src17"
OUT="$WORK/out17"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "The quick brown fox jumps over the lazy dog. Xdelta3 delta encoding test line." | head -c 65536 > "$SRC/delta_file.txt"
ORIG_SIZE=$(stat -c '%s' "$SRC/delta_file.txt")
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'XYZ' | dd of="$SRC/delta_file.txt" bs=1 seek=1000 conv=notrunc status=none
touch "$SRC/delta_file.txt"
assert_ok "create snapshot 2 (delta expected)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
assert_ok "restore snapshot 2" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "delta_file.txt byte-identical after restore" cmp -s "$SRC/delta_file.txt" "$BASE/delta_file.txt"
assert_ok "verify after delta" "$BARESNAP" verify "$REPO"
DIFF_OUT=$("$BARESNAP" diff "$REPO" "$SNAP1" "$SNAP2" 2>&1)
if printf '%s
' "$DIFF_OUT" | grep -q "M.*delta_file\.txt"; then pass "diff detects modification of delta_file.txt"; else fail "diff does not detect modification of delta_file.txt"; fi
RESTORED_SIZE=$(stat -c '%s' "$BASE/delta_file.txt")
assert_eq "file size preserved" "$ORIG_SIZE" "$RESTORED_SIZE"

