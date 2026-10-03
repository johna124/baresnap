# ============================================================
# 02. Large random file
# ============================================================
section "02. Large random file (2 MB)"
REPO="$WORK/repo02"
SRC="$WORK/src02"
OUT="$WORK/out02"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 2097152 /dev/urandom > "$SRC/big.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
assert_ok "big.bin byte-identical" cmp -s "$SRC/big.bin" "$BASE/big.bin"
assert_ok "verify" "$BARESNAP" verify "$REPO"

