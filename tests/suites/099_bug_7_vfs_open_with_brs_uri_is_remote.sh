# ============================================================
# 99. Bug #7: VFS open with brs_uri_is_remote()
# ============================================================
section "99. Bug #7: VFS open with brs_uri_is_remote()"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_T_B7="$(whoami)@localhost"
RP_B7="/tmp/smoke2_b7_repo"
URI_B7="ssh://${SSH_T_B7}${RP_B7}"
SRC_B7="$WORK/src_b7"
mkdir -p "$SRC_B7"
printf 'vfs open test
' > "$SRC_B7/file.txt"
head -c 16384 /dev/urandom > "$SRC_B7/blob.bin"
ssh "$SSH_T_B7" "rm -rf $RP_B7" 2>/dev/null || true
assert_ok "99.1 remote install" "$BARESNAP" remote install "$URI_B7"
assert_ok "99.2 remote init" "$BARESNAP" init "$URI_B7"
assert_ok "99.3 remote create" "$BARESNAP" create "$URI_B7" "$SRC_B7"
LIST_B7=$("$BARESNAP" list "$URI_B7" 2>/dev/null | grep '\.snap$' | wc -l)
assert_eq "99.4 remote list shows 1 snapshot" "1" "$LIST_B7"
set +e
"$BARESNAP" info "$URI_B7" >/dev/null 2>&1 </dev/null
INFO_RC_B7=$?
set -e
assert_eq "99.5 remote info works" "0" "$INFO_RC_B7"
assert_ok "99.6 remote verify" "$BARESNAP" verify "$URI_B7"
SNAP_B7=$("$BARESNAP" list "$URI_B7" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
set +e
"$BARESNAP" diff "$URI_B7" "$SNAP_B7" "$SNAP_B7" >/dev/null 2>&1 </dev/null
DIFF_RC_B7=$?
set -e
assert_eq "99.7 remote diff works" "0" "$DIFF_RC_B7"
ssh "$SSH_T_B7" "rm -rf $RP_B7" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted for Bug #7"
fi

