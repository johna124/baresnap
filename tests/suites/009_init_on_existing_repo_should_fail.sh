# ============================================================
# 09. Init on existing repo (should fail)
# ============================================================
section "09. Init on existing repo (should fail)"
REPO="$WORK/repo09"
assert_ok "init" "$BARESNAP" init "$REPO"
if "$BARESNAP" init "$REPO" >/dev/null 2>&1; then
fail "init on existing repo should fail"
else
pass "init on existing repo fails correctly"
fi

