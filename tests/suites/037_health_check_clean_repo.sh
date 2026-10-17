# ============================================================
# 37. Health check: clean repo
# ============================================================
section "37. Health check: clean repo detects no anomalies"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
REPO="$WORK/repo37"
SRC="$WORK/src37"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'health test
' > "$SRC/test.txt"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
HEALTH_OUT=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" </dev/null 2>&1)
HEALTH_RC=$?
if printf '%s
' "$HEALTH_OUT" | grep -q "no anomalies"; then
pass "health: clean repo without anomalies"
else
fail "health: clean repo reports unexpected anomalies"
fi
fi

