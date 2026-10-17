# ============================================================
# 38. Health check: detects orphan idx (idx without pack)
# ============================================================
section "38. Health check: detects index segment without pack"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
REPO="$WORK/repo38"
SRC="$WORK/src38"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'orphan idx test
' > "$SRC/test.txt"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
PACK=$(find "$REPO/packs" -maxdepth 1 -type f -name '*.pack' -print -quit 2>/dev/null) || true
if [ -n "$PACK" ]; then
rm -f -- "$PACK"
HEALTH_RC=0
HEALTH_OUT=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" </dev/null 2>&1) || HEALTH_RC=$?
if [ "$HEALTH_RC" -ge 128 ]; then
fail "health terminated by signal (rc=$HEALTH_RC)"
elif printf '%s
' "$HEALTH_OUT" | grep -q "sin pack"; then
pass "health detects orphan idx (deleted pack)"
else
fail "health does not detect orphan idx (rc=$HEALTH_RC)"
fi
else
fail "no pack found to delete"
fi
fi

