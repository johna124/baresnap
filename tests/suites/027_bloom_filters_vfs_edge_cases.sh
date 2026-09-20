# ============================================================
# 27. Bloom Filters + VFS + Edge Cases
# ============================================================
section "27. Bloom Filters + VFS + Edge Cases"
REPO="$WORK/repo27"
SRC="$WORK/src27"
OUT="$WORK/out27"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'bloom test data
' > "$SRC/data.txt"
head -c 65536 /dev/urandom > "$SRC/blob.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
BLM_COUNT=$(ls "$REPO/index/"*.blm 2>/dev/null | wc -l)
if [ "$BLM_COUNT" -ge 1 ]; then pass "bloom filter exists after create ($BLM_COUNT .blm)"; else fail "bloom filter DOES NOT exist after create"; fi
BLM_FILE=$(ls "$REPO/index/"*.blm 2>/dev/null | head -1)
if [ -n "$BLM_FILE" ]; then
BLM_MAGIC=$(head -c 8 "$BLM_FILE")
assert_eq "bloom filter magic is BRSBLM01" "BRSBLM01" "$BLM_MAGIC"
else
fail "no .blm found to verify magic"
fi
REPO_EMPTY="$WORK/repo27_empty"
SRC_EMPTY="$WORK/src27_empty"
OUT_EMPTY="$WORK/out27_empty"
BASE_EMPTY="$OUT_EMPTY/$(basename "$SRC_EMPTY")"
assert_ok "init (empty file)" "$BARESNAP" init "$REPO_EMPTY"
mkdir -p "$SRC_EMPTY"
touch "$SRC_EMPTY/empty.txt"
printf 'non-empty
' > "$SRC_EMPTY/normal.txt"
assert_ok "create with empty file" "$BARESNAP" create "$REPO_EMPTY" "$SRC_EMPTY"
LATEST_EMPTY=$(get_latest_snap "$REPO_EMPTY")
assert_ok "restore with empty file" "$BARESNAP" restore "$REPO_EMPTY" "$LATEST_EMPTY" "$OUT_EMPTY"
if [ -f "$BASE_EMPTY/empty.txt" ]; then
EMPTY_SIZE=$(stat -c '%s' "$BASE_EMPTY/empty.txt")
assert_eq "empty file restored with size 0" "0" "$EMPTY_SIZE"
else
fail "empty file does not exist after restore"
fi
assert_ok "normal file restored alongside empty" cmp -s "$SRC_EMPTY/normal.txt" "$BASE_EMPTY/normal.txt"
REPO_PRUNE="$WORK/repo27_prune"
SRC_PRUNE="$WORK/src27_prune"
assert_ok "init (prune bloom)" "$BARESNAP" init "$REPO_PRUNE"
mkdir -p "$SRC_PRUNE"
printf 'v1
' > "$SRC_PRUNE/f.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO_PRUNE" "$SRC_PRUNE"
sleep 1.1
printf 'v2
' > "$SRC_PRUNE/f.txt"
assert_ok "create snap 2" "$BARESNAP" create "$REPO_PRUNE" "$SRC_PRUNE"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO_PRUNE" --keep-last 1
assert_ok "verify after prune (bloom intact)" "$BARESNAP" verify "$REPO_PRUNE"
REPO_ZSTD="$WORK/repo27_zstd"
SRC_ZSTD="$WORK/src27_zstd"
OUT_ZSTD="$WORK/out27_zstd"
BASE_ZSTD="$OUT_ZSTD/$(basename "$SRC_ZSTD")"
assert_ok "init zstd level 7" "$BARESNAP" init "$REPO_ZSTD" --compression zstd --zstd-level 7
mkdir -p "$SRC_ZSTD"
for i in $(seq 1 5000); do echo "ZSTD level 7 roundtrip test line $i: The quick brown fox jumps over the lazy dog."; done > "$SRC_ZSTD/test.txt"
assert_ok "create zstd level 7" "$BARESNAP" create "$REPO_ZSTD" "$SRC_ZSTD"
LATEST_ZSTD=$(get_latest_snap "$REPO_ZSTD")
assert_ok "restore zstd level 7" "$BARESNAP" restore "$REPO_ZSTD" "$LATEST_ZSTD" "$OUT_ZSTD"
assert_ok "zstd level 7 content correct" cmp -s "$SRC_ZSTD/test.txt" "$BASE_ZSTD/test.txt"
assert_ok "verify zstd level 7" "$BARESNAP" verify "$REPO_ZSTD"
INFO_ZSTD7=$("$BARESNAP" info "$REPO_ZSTD" 2>&1)
if printf '%s
' "$INFO_ZSTD7" | grep -qi "zstd"; then pass "info shows compression: zstd (level 7)"; else fail "info does not show compression: zstd"; fi

