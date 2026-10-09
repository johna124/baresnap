# ============================================================
# 81. Inconsistent repo: directories exist but config is missing
# ============================================================
section "81. Inconsistent repo (config manually deleted)"
REPO81="$WORK/repo81"
SRC81="$WORK/src81"
mkdir -p "$SRC81"
printf 'test data' > "$SRC81/file.txt"
# Create healthy repo
assert_ok "81.1 init healthy repo" "$BARESNAP" init "$REPO81"
assert_ok "81.2 create first snapshot" "$BARESNAP" create "$REPO81" "$SRC81"
# Simulate corruption: delete config but leave directories
rm -f "$REPO81/config"
# Attempting create should FAIL with a clear message
set +e
CREATE_OUT81=$("$BARESNAP" create "$REPO81" "$SRC81" 2>&1)
CREATE_RC81=$?
set -e
if [ "$CREATE_RC81" -ne 0 ] && echo "$CREATE_OUT81" | grep -q "inconsistent state"; then
pass "81.3 create detected corrupt repo and aborted with clear message"
else
fail "81.3 create did not detect corruption (rc=$CREATE_RC81)"
log " [OUTPUT] $CREATE_OUT81"
fi
# Verify that health --repair fixes it
assert_ok "81.4 health --repair recovers the repo" "$BARESNAP" health "$REPO81" --repair
assert_ok "81.5 create works after repair" "$BARESNAP" create "$REPO81" "$SRC81"

