# ============================================================
# 54. SSH: Delta Encoding + Concurrent Prune over network
# ============================================================
section "54. SSH: Delta Encoding + Concurrent Prune over network"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_TARGET="$(whoami)@localhost"
REMOTE_PATH_DELTA="/tmp/baresnap_mega_test_delta"
SSH_URI_DELTA="ssh://${SSH_TARGET}${REMOTE_PATH_DELTA}"
SSH_SRC_DELTA="$WORK/ssh_src_delta"
SSH_OUT_DELTA="$WORK/ssh_out_delta"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_DELTA" 2>/dev/null || true
mkdir -p "$SSH_SRC_DELTA"
# Create repetitive text file (perfect candidate for xdelta3)
yes "BareSnap SSH Delta Stress Test Line. " | head -c 65536 > "$SSH_SRC_DELTA/delta.txt"
assert_ok "54.1 remote install (delta)" "$BARESNAP" remote install "$SSH_URI_DELTA"
assert_ok "54.2 remote init (delta)" "$BARESNAP" init "$SSH_URI_DELTA"
assert_ok "54.3 create snap 1 (base) via SSH" "$BARESNAP" create "$SSH_URI_DELTA" "$SSH_SRC_DELTA"
sleep 1.1
# Modify only 10 bytes in the middle (forces delta, not full re-chunking)
printf 'DELTA_MOD' | dd of="$SSH_SRC_DELTA/delta.txt" bs=1 seek=1000 conv=notrunc status=none
touch "$SSH_SRC_DELTA/delta.txt"
assert_ok "54.4 create snap 2 (delta) via SSH" "$BARESNAP" create "$SSH_URI_DELTA" "$SSH_SRC_DELTA"
sleep 1.1
# Modify again
printf 'DELTA_MOD_2' | dd of="$SSH_SRC_DELTA/delta.txt" bs=1 seek=2000 conv=notrunc status=none
touch "$SSH_SRC_DELTA/delta.txt"
assert_ok "54.5 create snap 3 (delta) via SSH" "$BARESNAP" create "$SSH_URI_DELTA" "$SSH_SRC_DELTA"
# Verify that remote info works correctly over SSH
# NOTE: Delta encoding is DISABLED over SSH by design.
# The cost of reconstructing previous versions via remote RPCs
# (hundreds of RPCs per file) does not compensate for the marginal savings
# of space (~100-200 KB in typical backups). For delta + SSH,
# use local backup + rsync to the server.
SSH_INFO_DELTA=$("$BARESNAP" info "$SSH_URI_DELTA" 2>/dev/null)
SSH_INFO_RC=$?
if [ "$SSH_INFO_RC" -eq 0 ] && printf '%s
' "$SSH_INFO_DELTA" | grep -q "Repository"; then
pass "54.6 remote info works correctly over SSH"
else
fail "54.6 remote info fails over SSH (rc=$SSH_INFO_RC)"
fi
# Execute PRUNE over SSH (this forces workers to rewrite packs and upload them via the SSH pipe)
assert_ok "54.7 prune --keep-last 1 via SSH (rewrite stress)" "$BARESNAP" prune "$SSH_URI_DELTA" --keep-last 1
# Verify integrity after remote prune
assert_ok "54.8 verify after remote prune" "$BARESNAP" verify "$SSH_URI_DELTA"
# Restore the surviving snapshot and check that the delta was rebuilt correctly
SSH_SNAP_DELTA=$("$BARESNAP" list "$SSH_URI_DELTA" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
rm -rf "$SSH_OUT_DELTA"
assert_ok "54.9 restore after remote prune" "$BARESNAP" restore "$SSH_URI_DELTA" "$SSH_SNAP_DELTA" "$SSH_OUT_DELTA"
SSH_BASE_DELTA="$SSH_OUT_DELTA/$(basename "$SSH_SRC_DELTA")"
assert_ok "54.10 delta.txt byte-identical after restore+prune SSH" cmp -s "$SSH_SRC_DELTA/delta.txt" "$SSH_BASE_DELTA/delta.txt"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_DELTA" 2>/dev/null || true
else
section "54. SSH: Delta + Prune"
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi

