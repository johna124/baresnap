# ============================================================
# 25. ZSTD: alternative compression
# ============================================================
section "25. ZSTD: alternative compression"
REPO_LZ4="$WORK/repo_lz4"
REPO_ZSTD="$WORK/repo_zstd"
SRC="$WORK/src25"
OUT_LZ4="$WORK/out_lz4"
OUT_ZSTD="$WORK/out_zstd"
mkdir -p "$SRC"
for i in $(seq 1 20000); do echo "Line $i: The quick brown fox jumps over the lazy dog. BareSnap ZSTD vs LZ4 compression benchmark test with entropy: $((i * 7 % 1000))"; done > "$SRC/test.txt"
for i in $(seq 1 5000); do echo "{\"id\":$i,\"name\":\"item_$i\",\"value\":$((i*3)),\"tags\":[\"backup\",\"dedup\",\"chunk\"]}"; done > "$SRC/data.json"
assert_ok "init LZ4" "$BARESNAP" init "$REPO_LZ4"
assert_ok "init ZSTD level 10" "$BARESNAP" init "$REPO_ZSTD" --compression zstd --zstd-level 10
assert_ok "create LZ4" "$BARESNAP" create "$REPO_LZ4" "$SRC"
assert_ok "create ZSTD" "$BARESNAP" create "$REPO_ZSTD" "$SRC"
PACK_LZ4=$(ls "$REPO_LZ4/packs/"*.pack 2>/dev/null | head -1)
PACK_ZSTD=$(ls "$REPO_ZSTD/packs/"*.pack 2>/dev/null | head -1)
if [ -n "$PACK_LZ4" ] && [ -n "$PACK_ZSTD" ]; then
SIZE_LZ4=$(stat -c '%s' "$PACK_LZ4")
SIZE_ZSTD=$(stat -c '%s' "$PACK_ZSTD")
log "  [INFO] LZ4: ${SIZE_LZ4} bytes | ZSTD: ${SIZE_ZSTD} bytes"
fi
LATEST_LZ4=$(get_latest_snap "$REPO_LZ4")
LATEST_ZSTD=$(get_latest_snap "$REPO_ZSTD")
assert_ok "restore LZ4" "$BARESNAP" restore "$REPO_LZ4" "$LATEST_LZ4" "$OUT_LZ4"
assert_ok "restore ZSTD" "$BARESNAP" restore "$REPO_ZSTD" "$LATEST_ZSTD" "$OUT_ZSTD"
BASE_LZ4="$OUT_LZ4/$(basename "$SRC")"
BASE_ZSTD="$OUT_ZSTD/$(basename "$SRC")"
assert_ok "LZ4 content correct (test.txt)" cmp -s "$SRC/test.txt" "$BASE_LZ4/test.txt"
assert_ok "LZ4 content correct (data.json)" cmp -s "$SRC/data.json" "$BASE_LZ4/data.json"
assert_ok "ZSTD content correct (test.txt)" cmp -s "$SRC/test.txt" "$BASE_ZSTD/test.txt"
assert_ok "ZSTD content correct (data.json)" cmp -s "$SRC/data.json" "$BASE_ZSTD/data.json"
assert_ok "verify LZ4" "$BARESNAP" verify "$REPO_LZ4"
assert_ok "verify ZSTD" "$BARESNAP" verify "$REPO_ZSTD"
INFO_LZ4=$("$BARESNAP" info "$REPO_LZ4" 2>&1)
INFO_ZSTD=$("$BARESNAP" info "$REPO_ZSTD" 2>&1)
if printf '%s
' "$INFO_LZ4" | grep -qi "lz4"; then pass "info shows compression: lz4"; else fail "info does not show compression: lz4"; fi
if printf '%s
' "$INFO_ZSTD" | grep -qi "zstd"; then pass "info shows compression: zstd"; else fail "info does not show compression: zstd"; fi

