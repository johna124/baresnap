# ============================================================
# 85. Index Grow: 2000 small files without recursive OOM
# ============================================================
section "85. Index Grow: 2000 small files without recursive OOM"
REPO85="$WORK/repo85"
SRC85="$WORK/src85"
OUT85="$WORK/out85"
BASE85="$OUT85/$(basename "$SRC85")"
mkdir -p "$SRC85"
assert_ok "85.1 init" "$BARESNAP" init "$REPO85"
# Generate 2000 small files to force hash table expansion
for i in $(seq 1 2000); do
printf "data-%04d" "$i" > "$SRC85/file_$i.txt"
done
pass "85.2 2000 small files created"
# Massive backup: forces index_map_grow without recursive OOM
assert_ok "85.3 massive create (2000 files, forces index grow)" "$BARESNAP" create "$REPO85" "$SRC85"
# Verify consistency
assert_ok "85.4 verify after massive backup" "$BARESNAP" verify "$REPO85"
# Restore and validate integrity
LATEST85=$(get_latest_snap "$REPO85")
assert_ok "85.5 massive restore" "$BARESNAP" restore "$REPO85" "$LATEST85" "$OUT85"
RESTORED85=$(find "$BASE85" -type f 2>/dev/null | wc -l)
assert_eq "85.6 2000 files restored" "2000" "$RESTORED85"
# Verify content of a random file
SAMPLE85=$(cat "$BASE85/file_1000.txt")
assert_eq "85.7 correct content" "data-1000" "$SAMPLE85"

