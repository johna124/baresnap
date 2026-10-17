# ============================================================
# 30. Selective extract: only file with delta from a mixed snapshot
# ============================================================
section "30. Selective extract of file with delta"
REPO="$WORK/repo30"
SRC="$WORK/src30"
OUT="$WORK/out30"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC/sub"
printf 'stable content
' > "$SRC/stable.txt"
head -c 32768 /dev/urandom > "$SRC/sub/blob.bin"
yes "selective extract delta test content" | head -c 49152 > "$SRC/changing.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'CHANGED
' >> "$SRC/changing.txt"; touch "$SRC/changing.txt"
assert_ok "create snap 2 (changing.txt delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT"
SRCBASE=$(basename "$SRC")
assert_ok "extract only changing.txt" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT" "$SRCBASE/changing.txt"
FOUND=$(find "$OUT" -name "changing.txt" -type f 2>/dev/null | head -1)
if [ -z "$FOUND" ]; then
fail "selective extract did not find changing.txt"
else
assert_ok "changing.txt byte-identical" cmp -s "$SRC/changing.txt" "$FOUND"
COUNT=$(find "$OUT" -type f 2>/dev/null | wc -l)
assert_eq "only 1 file extracted" "1" "$COUNT"
fi

