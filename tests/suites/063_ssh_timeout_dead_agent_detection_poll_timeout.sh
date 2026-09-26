# ============================================================
# 63. SSH Timeout: dead agent detection (poll + timeout)
# ============================================================
# Functional test if SSH is available
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_TARGET="$(whoami)@localhost"
REMOTE_PATH63="/tmp/baresnap_mega_test_timeout"
SSH_URI63="ssh://${SSH_TARGET}${REMOTE_PATH63}"
SSH_SRC63="$WORK/ssh_src63"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH63" 2>/dev/null || true
mkdir -p "$SSH_SRC63"
printf 'timeout test
' > "$SSH_SRC63/file.txt"
head -c 32768 /dev/urandom > "$SSH_SRC63/blob.bin"
assert_ok "63.5 remote install" "$BARESNAP" remote install "$SSH_URI63"
assert_ok "63.6 remote init" "$BARESNAP" init "$SSH_URI63"
assert_ok "63.7 remote create (timeout active)" "$BARESNAP" create "$SSH_URI63" "$SSH_SRC63"
assert_ok "63.8 remote verify" "$BARESNAP" verify "$SSH_URI63"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH63" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH functional test omitted"
fi

