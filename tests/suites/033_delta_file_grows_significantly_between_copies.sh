# ============================================================
# 33. Delta: file grows significantly between copies
# ============================================================
section "33. Delta: file grows significantly between copies"
REPO="$WORK/repo33"
SRC="$WORK/src33"
OUT="$WORK/out33"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "GROWTH TEST base content" | head -c 32768 > "$SRC/grow.txt"
assert_ok "create snap 1 (32KB)" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
yes "GROWTH TEST appended content for delta" | head -c 16384 >> "$SRC/grow.txt"
truncate -s 49152 "$SRC/grow.txt" 2>/dev/null || head -c 49152 /dev/zero >> "$SRC/grow.txt"
touch "$SRC/grow.txt"
GROW_SIZE=$(stat -c '%s' "$SRC/grow.txt")
assert_ok "create snap 2 (growth)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore snap 2" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
REST_SIZE=$(stat -c '%s' "$BASE/grow.txt")
assert_eq "correct size after restore" "$GROW_SIZE" "$REST_SIZE"
assert_ok "grow.txt byte-identical" cmp -s "$SRC/grow.txt" "$BASE/grow.txt"
assert_ok "verify after growth" "$BARESNAP" verify "$REPO"

