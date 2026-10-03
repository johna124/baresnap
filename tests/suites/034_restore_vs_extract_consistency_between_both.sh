# ============================================================
# 34. Restore vs Extract: consistency between both
# ============================================================
section "34. Restore vs Extract: both produce the same content"
REPO="$WORK/repo34"
SRC="$WORK/src34"
OUT_R="$WORK/out34_r"
OUT_E="$WORK/out34_e"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "CONSISTENCY TEST between restore and extract operations" | head -c 65536 > "$SRC/consist.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'CONSIST_MOD
' >> "$SRC/consist.txt"; touch "$SRC/consist.txt"
assert_ok "create snap 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT_R"
assert_ok "restore" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT_R"
R_FILE=$(find "$OUT_R" -name "consist.txt" -type f | head -1)
rm -rf "$OUT_E"
assert_ok "extract" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT_E"
E_FILE=$(find "$OUT_E" -name "consist.txt" -type f | head -1)
if [ -n "$R_FILE" ] && [ -n "$E_FILE" ]; then
assert_ok "restore == extract (byte-identical)" cmp -s "$R_FILE" "$E_FILE"
assert_ok "restore == source" cmp -s "$SRC/consist.txt" "$R_FILE"
assert_ok "extract == source" cmp -s "$SRC/consist.txt" "$E_FILE"
R_SIZE=$(stat -c '%s' "$R_FILE")
E_SIZE=$(stat -c '%s' "$E_FILE")
assert_eq "same size" "$R_SIZE" "$E_SIZE"
else
fail "restore or extract did not produce consist.txt"
fi

