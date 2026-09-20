# ============================================================
# 66. File Cache Self-Healing (Cache Healing)
# ============================================================
section "66. File Cache Self-Healing (Cache Healing)"
REPO66="$WORK/repo66"
SRC66="$WORK/src66"
OUT66="$WORK/out66"
BASE66="$OUT66/$(basename "$SRC66")"
assert_ok "66.1 init" "$BARESNAP" init "$REPO66"
mkdir -p "$SRC66"
printf 'cache healing test v1
' > "$SRC66/file.txt"
head -c 32768 /dev/urandom > "$SRC66/data.bin"
# First backup to generate the initial cache
assert_ok "66.2 create v1" "$BARESNAP" create "$REPO66" "$SRC66"
# Slightly modify to force delta in the next one
sleep 1.1
printf 'cache healing test v2
' > "$SRC66/file.txt"
assert_ok "66.3 create v2 (delta expected)" "$BARESNAP" create "$REPO66" "$SRC66"
# Verify that delta worked (small pack)
PACKS_BEFORE=$(du -sk "$REPO66/packs" | awk '{print $1}')
# CORRUPT THE CACHE: Inject garbage into the cache file
CACHE_FILE="$REPO66/cache"
if [ -f "$CACHE_FILE" ]; then
# Overwrite the beginning of the cache with zeros (breaks magic and entries)
dd if=/dev/zero of="$CACHE_FILE" bs=1 count=64 conv=notrunc status=none 2>/dev/null
pass "66.4 file cache artificially corrupted"
else
fail "66.4 no cache file found to corrupt"
fi
# Run another backup. The engine should detect the inconsistency,
# delete the corrupt cache and regenerate it from scratch.
# This backup will be slower (re-indexing) but must finish successfully.
assert_ok "66.5 create post-corruption (auto-heal)" "$BARESNAP" create "$REPO66" "$SRC66"
# Verify that the repo remains intact
assert_ok "66.6 verify after auto-heal" "$BARESNAP" verify "$REPO66"
# Restore and verify that the data is correct (delta should have worked after healing)
rm -rf "$OUT66"
LATEST_SNAP66=$(get_latest_snap "$REPO66")
assert_ok "66.7 restore after auto-heal" "$BARESNAP" restore "$REPO66" "$LATEST_SNAP66" "$OUT66"
assert_ok "66.8 correct content after auto-heal" cmp -s "$SRC66/file.txt" "$BASE66/file.txt"
# Verify that the cache has been regenerated (it is not empty or corrupt)
if [ -f "$CACHE_FILE" ]; then
CACHE_SIZE=$(stat -c '%s' "$CACHE_FILE")
if [ "$CACHE_SIZE" -gt 100 ]; then
pass "66.9 file cache regenerated correctly (${CACHE_SIZE} bytes)"
else
fail "66.9 regenerated file cache is suspiciously small (${CACHE_SIZE} bytes)"
fi
else
fail "66.9 file cache was not regenerated"
fi

