# ============================================================
# 21. Delta: size threshold (>256 KB → normal chunking)
# ============================================================
section "21. Delta: size threshold (>256 KB → normal chunking)"
REPO="$WORK/repo21"
SRC="$WORK/src21"
OUT="$WORK/out21"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 307200 /dev/urandom > "$SRC/large.bin"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'ABC' | dd of="$SRC/large.bin" bs=1 seek=1000 conv=notrunc status=none
touch "$SRC/large.bin"
assert_ok "create snapshot 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "large.bin byte-identical" cmp -s "$SRC/large.bin" "$BASE/large.bin"
assert_ok "verify" "$BARESNAP" verify "$REPO"

