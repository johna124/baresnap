# ============================================================
# 29. Delta chain: 4 copies with successive modifications
# ============================================================
section "29. Delta chain: 4 copies with successive modifications"
REPO="$WORK/repo29"
SRC="$WORK/src29"
OUT="$WORK/out29"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "BASE LINE for delta chain testing. This content will be modified incrementally." | head -c 65536 > "$SRC/chain.txt"
assert_ok "create snap 1 (base)" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'MOD_C2
' >> "$SRC/chain.txt"; touch "$SRC/chain.txt"
assert_ok "create snap 2 (delta vs 1)" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'MOD_C3
' >> "$SRC/chain.txt"; touch "$SRC/chain.txt"
assert_ok "create snap 3 (delta vs 2)" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'MOD_C4
' >> "$SRC/chain.txt"; touch "$SRC/chain.txt"
assert_ok "create snap 4 (delta vs 3)" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify after 4 copies with delta chain" "$BARESNAP" verify "$REPO"
SNAP_LAST=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore snap 4 (delta^3)" "$BARESNAP" restore "$REPO" "$SNAP_LAST" "$OUT"
assert_ok "chain.txt byte-identical after restore" cmp -s "$SRC/chain.txt" "$BASE/chain.txt"
rm -rf "$OUT"
assert_ok "extract snap 4 (delta^3)" "$BARESNAP" extract "$REPO" "$SNAP_LAST" "$OUT"
if [ -f "$BASE/chain.txt" ]; then
assert_ok "chain.txt byte-identical after extract" cmp -s "$SRC/chain.txt" "$BASE/chain.txt"
else
fail "extract did not produce chain.txt"
fi

