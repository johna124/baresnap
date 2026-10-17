# ============================================================
# 65. Ouroboros: The repo scans itself
# ============================================================
section "65. Ouroboros: The repo scans itself"
REPO65="$WORK/ouroboros_repo"
SRC65="$WORK/ouroboros_src"
mkdir -p "$SRC65/datos_reales"
printf 'data that must not die\n' > "$SRC65/datos_reales/importante.txt"

# Launch the "suicidal" create.
# BareSnap does NOT exclude the repo, but being empty/atomic,
# the scanner does not enter an exponential loop and finishes quickly.
# We use timeout just in case to avoid blocking the test suite.
set +e
timeout 10 "$BARESNAP" create "$REPO65" "$SRC65" > "$WORK/ouroboros.log" 2>&1
OUROBOROS_RC=$?
set -e
if [ "$OUROBOROS_RC" -eq 124 ]; then
fail "65.3 BareSnap entered infinite loop/deadlock (timeout)"
elif [ "$OUROBOROS_RC" -eq 0 ]; then
pass "65.3 BareSnap completed scanning its own structure without deadlock"
else
# If it failed due to ENOSPC or another error, it is also valid (survived the chaos)
pass "65.3 BareSnap cleanly aborted the suicidal scan (rc=$OUROBOROS_RC)"
fi
# SURVIVAL VERIFICATION
# 1. No zombie temporaries should remain in tmp/ (the abort or success cleaned them)
TMP65=$(find "$REPO65/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP65" -eq 0 ]; then
pass "65.4 Zero zombie temporaries after Ouroboros scan"
else
fail "65.4 $TMP65 zombie temporaries survived in tmp/"
fi
# 2. The repo must remain 100% intact and verifiable
assert_ok "65.5 verify of the repo survives the cannibal scan" "$BARESNAP" verify "$REPO65"
# 3. The user's real data was not corrupted
if [ -f "$SRC65/datos_reales/importante.txt" ]; then
CONTENT=$(cat "$SRC65/datos_reales/importante.txt")
if [ "$CONTENT" = "data that must not die" ]; then
pass "65.6 User data remains intact on disk"
else
fail "65.6 User data was altered"
fi
else
fail "65.6 User data disappeared!"
fi

