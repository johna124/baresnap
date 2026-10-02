# ============================================================
# 98. Bug #6: VFS cleanup between operations
# ============================================================
section "98. Bug #6: VFS cleanup between operations"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_T_B6="$(whoami)@localhost"
RP_B6="/tmp/smoke2_b6_repo"
URI_B6="ssh://${SSH_T_B6}${RP_B6}"
SRC_B6="$WORK/src_b6"
OUT_B6="$WORK/out_b6"
mkdir -p "$SRC_B6"
printf 'vfs cleanup test
' > "$SRC_B6/file.txt"
ssh "$SSH_T_B6" "rm -rf $RP_B6" 2>/dev/null || true
assert_ok "98.1 init local" "$BARESNAP" init "$WORK/repo_b6_local"
assert_ok "98.2 create local" "$BARESNAP" create "$WORK/repo_b6_local" "$SRC_B6"
assert_ok "98.3 remote install" "$BARESNAP" remote install "$URI_B6"
assert_ok "98.4 remote init" "$BARESNAP" init "$URI_B6"
assert_ok "98.5 remote create" "$BARESNAP" create "$URI_B6" "$SRC_B6"
SNAP_LOCAL_B6=$(get_latest_snap "$WORK/repo_b6_local")
assert_ok "98.6 restore local after remote operation" "$BARESNAP" restore "$WORK/repo_b6_local" "$SNAP_LOCAL_B6" "$OUT_B6"
BASE_B6="$OUT_B6/$(basename "$SRC_B6")"
if [ -f "$BASE_B6/file.txt" ] && grep -q "vfs cleanup test" "$BASE_B6/file.txt" 2>/dev/null; then
pass "98.7 correct content after local→remote→local sequence"
else
fail "98.7 incorrect content after local→remote→local sequence"
fi
assert_ok "98.8 remote verify after local operations" "$BARESNAP" verify "$URI_B6"
ssh "$SSH_T_B6" "rm -rf $RP_B6" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted for Bug #6"
fi

