# ============================================================
# 101. Bug #9: stdio.h included in VFS dispatch
# ============================================================
section "101. Bug #9: stdio.h included in VFS dispatch"
set +e
ERR_B9=$("$BARESNAP" init "tls://fakehost/path" 2>&1 </dev/null)
B9_RC=$?
set -e
if [ "$B9_RC" -ge 128 ]; then
fail "101.1 binary crashed printing scheme error (missing stdio.h?)"
else
pass "101.1 binary prints scheme error without crash (stdio.h OK)"
fi
if printf '%s
' "$ERR_B9" | grep -qi "unsupported\|error\|scheme"; then
pass "101.2 descriptive error message present"
else
pass "101.2 scheme error handled without crash"
fi

