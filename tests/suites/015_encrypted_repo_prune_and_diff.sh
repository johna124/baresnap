# ============================================================
# 15. Encrypted repo: prune and diff
# ============================================================
section "15. Encrypted repo: prune and diff"
REPO="$WORK/repo15"
SRC="$WORK/src15"
mkdir -p "$SRC"
echo "v1" > "$SRC/file.txt"
export BARESNAP_PASSPHRASE="prune-test-pass"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
echo "v2" > "$SRC/file.txt"
assert_ok "create snapshot 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
SNAPS=$(get_snap_count "$REPO")
assert_eq "2 encrypted snapshots" "2" "$SNAPS"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
SNAP_COUNT=$(get_snap_count "$REPO")
assert_eq "1 snapshot after prune" "1" "$SNAP_COUNT"
assert_ok "verify after prune" "$BARESNAP" verify "$REPO"
unset BARESNAP_PASSPHRASE

