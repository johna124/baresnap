# ============================================================
# 90. LIST with deep path (20 levels)
# ============================================================
section "90. LIST with deep path (20 levels)"
REPO90="$WORK/repo90"
SRC90="$WORK/src90"
OUT90="$WORK/out90"
DEEP90="$SRC90"
for i in $(seq 1 20); do
DEEP90="$DEEP90/level_${i}_directory_name_padding"
done
mkdir -p "$DEEP90"
printf 'deep content
' > "$DEEP90/deep_file.txt"
assert_ok "90.1 init" "$BARESNAP" init "$REPO90"
assert_ok "90.2 create with deep path" "$BARESNAP" create "$REPO90" "$SRC90"
SNAP90=$(get_latest_snap "$REPO90")
assert_ok "90.3 ls recursive with deep path" "$BARESNAP" ls "$REPO90" "$SNAP90" --recursive
assert_ok "90.4 restore with deep path" "$BARESNAP" restore "$REPO90" "$SNAP90" "$OUT90"
DEEP_FILE90=$(find "$OUT90" -name "deep_file.txt" -type f 2>/dev/null | head -1)
if [ -n "$DEEP_FILE90" ]; then
CONTENT90=$(cat "$DEEP_FILE90")
assert_eq "90.5 deep file content correct" "deep content" "$CONTENT90"
else
fail "90.5 deep file not found after restore"
fi
assert_ok "90.6 verify with deep path" "$BARESNAP" verify "$REPO90"

