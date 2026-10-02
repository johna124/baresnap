# ============================================================
# 05. Cross-snapshot dedup
# ============================================================
section "05. Cross-snapshot dedup"
REPO="$WORK/repo05"
SRC="$WORK/src05"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 262144 /dev/urandom > "$SRC/data.bin"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
PACKS_BEFORE=$(ls "$REPO/packs/" 2>/dev/null | wc -l)
SIZE_BEFORE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
assert_ok "create snapshot 2 (identical)" "$BARESNAP" create "$REPO" "$SRC"
SIZE_AFTER=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
SNAPS=$(get_snap_count "$REPO")
assert_eq "2 snapshots exist" "2" "$SNAPS"
GROWTH=$((SIZE_AFTER - SIZE_BEFORE))
if [ "$GROWTH" -le 64 ]; then
pass "cross-snapshot dedup (growth ${GROWTH} KB <= 64 KB)"
else
fail "cross-snapshot dedup (growth ${GROWTH} KB > 64 KB)"
fi

