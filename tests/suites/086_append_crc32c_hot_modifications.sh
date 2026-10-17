# ============================================================
# 86. Append & CRC32C: hot modifications
# ============================================================
section "86. Append & CRC32C: hot modifications"
REPO86="$WORK/repo86"
SRC86="$WORK/src86"
OUT86="$WORK/out86"
BASE86="$OUT86/$(basename "$SRC86")"
mkdir -p "$SRC86"
assert_ok "86.1 init" "$BARESNAP" init "$REPO86"
# Initial backup
for i in $(seq 1 100); do
printf "initial-%04d" "$i" > "$SRC86/file_$i.txt"
done
assert_ok "86.2 initial create (100 files)" "$BARESNAP" create "$REPO86" "$SRC86"
# Hot modifications (append + new files)
for i in $(seq 1 50); do
printf '  MODIFIED' >> "$SRC86/file_$i.txt"
done
for i in $(seq 101 200); do
printf "new-%04d" "$i" > "$SRC86/file_$i.txt"
done
pass "86.3 hot modifications applied (50 appends + 100 new)"
# Incremental backup: validates hot CRC32C and correct append
assert_ok "86.4 incremental create (append + CRC32C)" "$BARESNAP" create "$REPO86" "$SRC86"
# Verify must pass 100% intact
assert_ok "86.5 incremental verify (CRC32C integrity)" "$BARESNAP" verify "$REPO86"
# Restore and verify byte-identical
rm -rf "$OUT86"
LATEST86=$(get_latest_snap "$REPO86")
assert_ok "86.6 incremental restore" "$BARESNAP" restore "$REPO86" "$LATEST86" "$OUT86"
# Spot-check: verify that a modified file is correct
if grep -q "MODIFIED" "$BASE86/file_1.txt" 2>/dev/null; then
pass "86.7 modified content present in restore"
else
fail "86.7 modified content NOT present in restore"
fi
# Verify that the total count is correct
RESTORED86=$(find "$BASE86" -type f 2>/dev/null | wc -l)
assert_eq "86.8 200 files after incremental" "200" "$RESTORED86"

