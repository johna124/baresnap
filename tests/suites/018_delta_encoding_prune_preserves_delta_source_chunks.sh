# ============================================================
# 18. Delta Encoding: prune preserves delta source chunks
# ============================================================
section "18. Delta Encoding: prune preserves source chunks"
REPO="$WORK/repo18"
SRC="$WORK/src18"
OUT="$WORK/out18"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "ABCDEFGHIJKLMNOPQRSTUVWXY - base content for delta prune test." | head -c 32768 > "$SRC/prune_delta.txt"
assert_ok "create snapshot 1 (base)" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'MOD' | dd of="$SRC/prune_delta.txt" bs=1 seek=500 conv=notrunc status=none
touch "$SRC/prune_delta.txt"
assert_ok "create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
SNAPS_BEFORE=$(get_snap_count "$REPO")
assert_eq "2 snapshots before prune" "2" "$SNAPS_BEFORE"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "1 snapshot after prune" "1" "$SNAPS_AFTER"
assert_ok "verify after prune with delta" "$BARESNAP" verify "$REPO"
assert_ok "restore snapshot 2 after prune" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "prune_delta.txt restored correctly" cmp -s "$SRC/prune_delta.txt" "$BASE/prune_delta.txt"
if head -c 503 "$BASE/prune_delta.txt" | tail -c 3 | grep -q "MOD"; then pass "modified bytes present in restore"; else fail "modified bytes NOT present in restore"; fi

