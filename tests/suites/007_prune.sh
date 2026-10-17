# ============================================================
# 07. Prune
# ============================================================
section "07. Prune"
REPO="$WORK/repo07"
SRC="$WORK/src07"
OUT="$WORK/out07"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
for i in 1 2 3; do
printf 'version %s
' "$i" > "$SRC/file.txt"
assert_ok "create snapshot $i" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
done
SNAPS_BEFORE=$(get_snap_count "$REPO")
assert_eq "3 snapshots before prune" "3" "$SNAPS_BEFORE"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "1 snapshot after prune" "1" "$SNAPS_AFTER"
assert_ok "verify after prune" "$BARESNAP" verify "$REPO"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore of remaining snapshot" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
RESTORED=$(cat "$BASE/file.txt")
assert_eq "restored content is version 3" "version 3" "$RESTORED"

