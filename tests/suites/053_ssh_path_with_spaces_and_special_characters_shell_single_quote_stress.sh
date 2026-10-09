# ============================================================
# 53. SSH: Path with spaces and special characters (shell_single_quote stress)
# ============================================================
section "53. SSH: Path with spaces and special characters"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_TARGET="$(whoami)@localhost"
# Path with spaces, dashes, and parentheses (shell escaping hell)
REMOTE_PATH_WEIRD="/tmp/baresnap test repo (weird) & stuff"
SSH_URI_WEIRD="ssh://${SSH_TARGET}${REMOTE_PATH_WEIRD}"
SSH_SRC_WEIRD="$WORK/ssh_src_weird"
SSH_OUT_WEIRD="$WORK/ssh_out_weird"
# Clean remote path (using single quotes in ssh so the remote shell interprets it)
ssh "$SSH_TARGET" "rm -rf '${REMOTE_PATH_WEIRD}'" 2>/dev/null || true
mkdir -p "$SSH_SRC_WEIRD"
printf 'weird path test
' > "$SSH_SRC_WEIRD/file.txt"
assert_ok "53.1 remote install in path with spaces" "$BARESNAP" remote install "$SSH_URI_WEIRD"
assert_ok "53.2 remote init in path with spaces" "$BARESNAP" init "$SSH_URI_WEIRD"
assert_ok "53.3 remote create in path with spaces" "$BARESNAP" create "$SSH_URI_WEIRD" "$SSH_SRC_WEIRD"
SSH_SNAP_WEIRD=$("$BARESNAP" list "$SSH_URI_WEIRD" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
rm -rf "$SSH_OUT_WEIRD"
assert_ok "53.4 remote restore from path with spaces" "$BARESNAP" restore "$SSH_URI_WEIRD" "$SSH_SNAP_WEIRD" "$SSH_OUT_WEIRD"
SSH_BASE_WEIRD="$SSH_OUT_WEIRD/$(basename "$SSH_SRC_WEIRD")"
assert_ok "53.5 identical content after restore from weird path" cmp -s "$SSH_SRC_WEIRD/file.txt" "$SSH_BASE_WEIRD/file.txt"
# Cleanup
ssh "$SSH_TARGET" "rm -rf '${REMOTE_PATH_WEIRD}'" 2>/dev/null || true
else
section "53. SSH: Path with spaces"
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi

