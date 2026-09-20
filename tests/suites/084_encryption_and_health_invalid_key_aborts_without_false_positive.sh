# ============================================================
# 84. Encryption and Health: invalid key aborts without false positive
# ============================================================
section "84. Encryption and Health: invalid key does not generate false positive"
REPO84="$WORK/repo84"
SRC84="$WORK/src84"
mkdir -p "$SRC84"
printf 'secret data for health test
' > "$SRC84/secret.txt"
head -c 32768 /dev/urandom > "$SRC84/blob.bin"
export BARESNAP_PASSPHRASE="correct-passphrase-84"
assert_ok "84.1 init --encrypt" "$BARESNAP" init "$REPO84" --encrypt
assert_ok "84.2 create encrypted" "$BARESNAP" create "$REPO84" "$SRC84"
assert_ok "84.3 verify encrypted (correct key)" "$BARESNAP" verify "$REPO84"
unset BARESNAP_PASSPHRASE
# Force health with INCORRECT key: must abort without false positives
export BARESNAP_PASSPHRASE="wrong-passphrase-84"
set +e
HEALTH_OUT84=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO84" </dev/null 2>&1)
HEALTH_RC84=$?
set -e
if [ "$HEALTH_RC84" -ge 128 ]; then
fail "84.4 health with incorrect key crashed by signal (rc=$HEALTH_RC84)"
elif [ "$HEALTH_RC84" -ne 0 ]; then
pass "84.4 health with incorrect key aborts cleanly (rc=$HEALTH_RC84)"
else
fail "84.4 health with incorrect key returned rc=0 (should fail)"
fi
# Verify that /damaged was NOT created (destructive false positive)
if [ -d "$REPO84/damaged" ]; then
DAMAGED84=$(find "$REPO84/damaged" -type f 2>/dev/null | wc -l)
if [ "$DAMAGED84" -gt 0 ]; then
fail "84.5 health moved healthy snapshots to damaged/ (false positive)"
else
pass "84.5 damaged/ exists but is empty"
fi
else
pass "84.5 damaged/ was not created (no destructive false positive)"
fi
# Verify that the repo remains intact with the correct key
export BARESNAP_PASSPHRASE="correct-passphrase-84"
assert_ok "84.6 verify still passes with correct key" "$BARESNAP" verify "$REPO84"
SNAP84=$(get_latest_snap "$REPO84")
assert_ok "84.7 restore still works" "$BARESNAP" restore "$REPO84" "$SNAP84" "$WORK/out84"
unset BARESNAP_PASSPHRASE

