# ============================================================
# 94. Bug #2: Partial write / truncated pack
# ============================================================
section "94. Bug #2: Partial write / truncated pack"
REPO_B2="$WORK/repo_b2"
SRC_B2="$WORK/src_b2"
mkdir -p "$SRC_B2"
head -c 65536 /dev/urandom > "$SRC_B2/upload_test.bin"
printf 'small file
' > "$SRC_B2/small.txt"
assert_ok "94.1 init" "$BARESNAP" init "$REPO_B2"
assert_ok "94.2 create" "$BARESNAP" create "$REPO_B2" "$SRC_B2"
assert_ok "94.3 verify before truncating" "$BARESNAP" verify "$REPO_B2"
PACK_B2=$(find "$REPO_B2/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | head -1)
if [ -n "$PACK_B2" ]; then
PACK_SIZE_B2=$(stat -c '%s' "$PACK_B2")
HALF_B2=$((PACK_SIZE_B2 / 2))
if command -v truncate >/dev/null 2>&1; then
truncate -s "$HALF_B2" "$PACK_B2"
pass "94.4 pack truncated to $HALF_B2 bytes (from $PACK_SIZE_B2) with truncate"
else
dd if="$PACK_B2" of="$PACK_B2.trunc" bs=1 count="$HALF_B2" 2>/dev/null
mv "$PACK_B2.trunc" "$PACK_B2"
pass "94.4 pack truncated to $HALF_B2 bytes (from $PACK_SIZE_B2) with dd"
fi
else
fail "94.4 no pack found to truncate"
fi
set +e
"$BARESNAP" verify "$REPO_B2" >/dev/null 2>&1 </dev/null
VERIFY_RC_B2=$?
set -e
if [ "$VERIFY_RC_B2" -ge 128 ]; then
fail "94.5 verify CRASHED with truncated pack (signal $((VERIFY_RC_B2 - 128)))"
elif [ "$VERIFY_RC_B2" -ne 0 ]; then
pass "94.5 verify detected truncated pack (rc=$VERIFY_RC_B2)"
else
fail "94.5 verify did NOT detect truncated pack"
fi
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_T_B2="$(whoami)@localhost"
RP_B2="/tmp/smoke2_b2_repo"
URI_B2="ssh://${SSH_T_B2}${RP_B2}"
ssh "$SSH_T_B2" "rm -rf $RP_B2" 2>/dev/null || true
assert_ok "94.6 remote install" "$BARESNAP" remote install "$URI_B2"
assert_ok "94.7 remote init" "$BARESNAP" init "$URI_B2"
assert_ok "94.8 remote create (full upload)" "$BARESNAP" create "$URI_B2" "$SRC_B2"
assert_ok "94.9 remote verify (intact upload)" "$BARESNAP" verify "$URI_B2"
ssh "$SSH_T_B2" "rm -rf $RP_B2" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted for Bug #2"
fi

