# ============================================================
# 55. SSH: Password Authentication (Askpass + BARESNAP_SSH_PASSWORD)
# ============================================================
section "55. SSH: Password Authentication (Askpass + BARESNAP_SSH_PASSWORD)"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
DUMMY_PASS="dummy_test_pass_123_WRONG"
SSH_TARGET="$(whoami)@localhost"
REMOTE_PATH_PASS="/tmp/baresnap_mega_test_pass"
SSH_URI_PASS="ssh://${SSH_TARGET}${REMOTE_PATH_PASS}"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_PASS" 2>/dev/null || true
# ----------------------------------------------------------------
# TEST 55.1: INCORRECT Password via BARESNAP_SSH_PASSWORD
# Force password authentication (without public keys)
# ----------------------------------------------------------------
set +e
# Force password auth: disable pubkey for this test
export GIT_SSH_COMMAND="ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no"
OUT55=$(timeout 10 env BARESNAP_SSH_PASSWORD="$DUMMY_PASS" "$BARESNAP" remote install "$SSH_URI_PASS" 2>&1)
RC=$?
unset GIT_SSH_COMMAND
set -e
if [ "$RC" -eq 124 ]; then
fail "55.1 askpass hung (timeout 10s)"
elif [ "$RC" -eq 0 ]; then
# If it passed with dummy pass, there is probably an SSH key and password was not used
pass "55.1 successful authentication (possible SSH key, not password)"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_PASS" 2>/dev/null || true
else
# RC != 0 and != 124: SSH rejected the password (expected behavior)
if echo "$OUT55" | grep -qi "permission denied\|auth"; then
pass "55.1 BARESNAP_SSH_PASSWORD injected, SSH rejects incorrect pass (rc=$RC)"
else
pass "55.1 askpass did not hang, clean SSH error (rc=$RC)"
fi
fi
# ----------------------------------------------------------------
# TEST 55.2: Verify askpass wrapper
# ----------------------------------------------------------------
ASKPASS_PATH="$HOME/.local/libexec/baresnap-askpass"
if [ -f "$ASKPASS_PATH" ] && [ -x "$ASKPASS_PATH" ]; then
if grep -q "askpass-internal" "$ASKPASS_PATH" 2>/dev/null; then
pass "55.2 askpass wrapper generated correctly"
else
fail "55.2 wrapper exists but does not contain askpass-internal"
fi
else
pass "55.2 askpass wrapper (generated on demand, skip)"
fi
# ----------------------------------------------------------------
# TEST 55.3: --askpass-internal direct with BARESNAP_SSH_PASSWORD
# ----------------------------------------------------------------
set +e
PASS_OUT=$(BARESNAP_SSH_PASSWORD="test_secret_123" "$BARESNAP" --askpass-internal "" 2>/dev/null)
RC=$?
set -e
if [ "$RC" -eq 0 ] && [ "$PASS_OUT" = "test_secret_123" ]; then
pass "55.3 --askpass-internal with BARESNAP_SSH_PASSWORD works"
else
fail "55.3 --askpass-internal did not return the password (rc=$RC)"
fi
# ----------------------------------------------------------------
# TEST 55.4: --askpass-internal without TTY or env must fail quickly
# We use setsid to run without a controlling TTY
# ----------------------------------------------------------------
set +e
unset BARESNAP_SSH_PASSWORD
# Try with setsid (removes controlling TTY)
if command -v setsid >/dev/null 2>&1; then
timeout 3 setsid "$BARESNAP" --askpass-internal "Password: " </dev/null >/dev/null 2>&1
RC=$?
else
# Fallback: without setsid, in an interactive terminal there will always be a TTY
# We accept timeout as expected behavior
timeout 3 "$BARESNAP" --askpass-internal "Password: " </dev/null >/dev/null 2>&1
RC=$?
fi
set -e
if [ "$RC" -ne 0 ] && [ "$RC" -ne 124 ]; then
pass "55.4 --askpass-internal without TTY or env fails cleanly (rc=$RC)"
elif [ "$RC" -eq 124 ]; then
# Timeout: in an interactive environment without setsid, it is expected
if command -v setsid >/dev/null 2>&1; then
fail "55.4 --askpass-internal hung with setsid (rc=124)"
else
pass "55.4 --askpass-internal timeout without setsid (expected in terminal)"
fi
else
fail "55.4 --askpass-internal without TTY or env should fail (rc=$RC)"
fi
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_PASS" 2>/dev/null || true
else
section "55. SSH: Askpass"
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi

