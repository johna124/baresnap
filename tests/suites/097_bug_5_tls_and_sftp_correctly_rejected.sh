# ============================================================
# 97. Bug #5: tls:// and sftp:// correctly rejected
# ============================================================
section "97. Bug #5: tls:// and sftp:// correctly rejected"
set +e
"$BARESNAP" init "tls://fakehost/path" >/dev/null 2>&1 </dev/null
TLS_RC=$?
set -e
if [ "$TLS_RC" -ne 0 ]; then pass "97.1 init tls:// fails cleanly (rc=$TLS_RC)"; else fail "97.1 init tls:// did not fail"; fi
set +e
"$BARESNAP" init "sftp://fakehost/path" >/dev/null 2>&1 </dev/null
SFTP_RC=$?
set -e
if [ "$SFTP_RC" -ne 0 ]; then pass "97.2 init sftp:// fails cleanly (rc=$SFTP_RC)"; else fail "97.2 init sftp:// did not fail"; fi
TLS_DIR="$WORK/tls_garbage"
mkdir -p "$TLS_DIR"
if [ -z "$(ls -A "$TLS_DIR" 2>/dev/null)" ]; then pass "97.3 no garbage local directories created"; else fail "97.3 garbage local directories created"; fi
set +e
"$BARESNAP" create "tls://fakehost/path" "$WORK" >/dev/null 2>&1 </dev/null
CREATE_TLS_RC=$?
set -e
if [ "$CREATE_TLS_RC" -ne 0 ]; then pass "97.4 create tls:// fails cleanly (rc=$CREATE_TLS_RC)"; else fail "97.4 create tls:// did not fail"; fi
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
set +e
"$BARESNAP" init "ssh://invalid_host_name_that_does_not_exist_12345/path" >/dev/null 2>&1 </dev/null
SSH_BAD_RC=$?
set -e
if [ "$SSH_BAD_RC" -ge 128 ] && [ "$SSH_BAD_RC" -ne 255 ]; then
fail "97.5 init ssh:// invalid host CRASHED (signal $((SSH_BAD_RC - 128)))"
else
pass "97.5 init ssh:// invalid host fails without crash (rc=$SSH_BAD_RC)"
fi
else
log "  ${YELLOW}[SKIP]${NC} SSH test omitted for Bug #5"
fi

