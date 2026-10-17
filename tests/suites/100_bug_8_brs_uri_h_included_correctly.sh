# ============================================================
# 100. Bug #8: brs_uri.h included correctly
# ============================================================
section "100. Bug #8: brs_uri.h included correctly"
assert_ok "100.1 binary executable" test -x "$BARESNAP"
set +e
"$BARESNAP" init "ssh://user@host/path" >/dev/null 2>&1 </dev/null
URI_RC=$?
set -e
if [ "$URI_RC" -ge 128 ] && [ "$URI_RC" -ne 255 ]; then
fail "100.2 binary crashed parsing URI (missing brs_uri symbol?)"
else
pass "100.2 binary parses URIs without crash (brs_uri.h OK)"
fi
assert_ok "100.3 --version works" "$BARESNAP" --version

