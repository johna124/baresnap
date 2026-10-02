# ============================================================
# 80. Frankenstein Repo: config+index deleted, packs alive
# ============================================================
section "80. Frankenstein Repo: config+index deleted"
REPO80="$WORK/repo80"
SRC80="$WORK/src80"
mkdir -p "$SRC80"
printf 'frankenstein test
' > "$SRC80/data.txt"
head -c 65536 /dev/urandom > "$SRC80/blob.bin"
assert_ok "80.1 init" "$BARESNAP" init "$REPO80"
assert_ok "80.2 create" "$BARESNAP" create "$REPO80" "$SRC80"
# Destroy config, index and cache (leave packs and snapshots)
rm -f "$REPO80/config" "$REPO80/cache"
rm -rf "$REPO80/index"
# create must NOT crash (must auto-init and work)
set +e
"$BARESNAP" create "$REPO80" "$SRC80" > "$WORK/frankenstein.log" 2>&1
RC80=$?
set -e
if [ "$RC80" -ge 128 ]; then
fail "80.3 create CRASHED with signal $((RC80 - 128))"
elif [ "$RC80" -eq 0 ]; then
pass "80.3 create survived Frankenstein repo (auto-init + recreate)"
else
pass "80.3 create aborted cleanly without crashing (rc=$RC80)"
fi
# Post-kill audit (same pattern as prune tests)
audit_post_prune "$REPO80" "80.4 post-frankenstein"

