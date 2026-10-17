# ============================================================
# 58. Time Machine: unique IDs and temporal consistency
# ============================================================
section "58. Time Machine: unique IDs and temporal consistency"
REPO58="$WORK/repo58"
SRC58="$WORK/src58"
assert_ok "58.1 init" "$BARESNAP" init "$REPO58"
mkdir -p "$SRC58"
# Create 10 snapshots in a rapid burst (without sleep) to stress brs_now_ns()
for i in 1 2 3 4 5 6 7 8 9 10; do
printf "time v$i
" > "$SRC58/file.txt"
"$BARESNAP" create "$REPO58" "$SRC58" >/dev/null 2>&1
done
SNAPS58=$(get_snap_count "$REPO58")
assert_eq "58.2 10 snapshots in rapid burst" "10" "$SNAPS58"
# Verify uniqueness of pack IDs (brs_now_ns must not collide)
PACK_IDS58=$(find "$REPO58/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | sed 's/.*\///;s/\.pack//' | sort)
TOTAL_PACKS58=$(echo "$PACK_IDS58" | grep -c '[0-9]' 2>/dev/null || echo 0)
UNIQUE_PACKS58=$(echo "$PACK_IDS58" | sort -u | grep -c '[0-9]' 2>/dev/null || echo 0)
if [ "$TOTAL_PACKS58" -eq "$UNIQUE_PACKS58" ] && [ "$TOTAL_PACKS58" -gt 0 ]; then
pass "58.3 unique pack IDs ($UNIQUE_PACKS58)"
else
fail "58.3 pack ID collision ($TOTAL_PACKS58 total vs $UNIQUE_PACKS58 unique)"
fi
# Prune and verify transactional consistency
assert_ok "58.4 prune --keep-last 2" "$BARESNAP" prune "$REPO58" --keep-last 2
assert_ok "58.5 verify after prune" "$BARESNAP" verify "$REPO58"
SNAPS58_AFTER=$(get_snap_count "$REPO58")
assert_eq "58.6 2 snapshots after prune" "2" "$SNAPS58_AFTER"
audit_post_prune "$REPO58" "58.7 post-prune"

