# ============================================================
# 39. Health check: detects missing chunks after deleting pack
# ============================================================
section "39. Health check: detects missing chunks in snapshots"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
REPO="$WORK/repo39"
SRC="$WORK/src39"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 65536 /dev/urandom > "$SRC/data.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
PACK=$(find "$REPO/packs" -maxdepth 1 -type f -name '*.pack' -print -quit 2>/dev/null) || true
if [ -n "$PACK" ]; then
rm -f -- "$PACK"
HEALTH_RC=0
HEALTH_OUT=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" </dev/null 2>&1) || HEALTH_RC=$?
if [ "$HEALTH_RC" -ge 128 ]; then
fail "health terminated by signal (rc=$HEALTH_RC)"
elif printf '%s
' "$HEALTH_OUT" | grep -q "chunk(s)"; then
pass "health detects chunks without index/pack"
else
fail "health does not detect missing chunks (rc=$HEALTH_RC)"
fi
else
fail "no pack found to delete"
fi
fi

