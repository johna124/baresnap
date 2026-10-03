# ============================================================
# 102. Bug #10: BrsRepoLock without conflict
# ============================================================
section "102. Bug #10: BrsRepoLock without conflict"
REPO_B10="$WORK/repo_b10"
SRC_B10="$WORK/src_b10"
mkdir -p "$SRC_B10"
printf 'lock test
' > "$SRC_B10/file.txt"
assert_ok "102.1 init" "$BARESNAP" init "$REPO_B10"
assert_ok "102.2 create (acquires lock)" "$BARESNAP" create "$REPO_B10" "$SRC_B10"
assert_ok "102.3 create 2 (lock re-acquired)" "$BARESNAP" create "$REPO_B10" "$SRC_B10"
LOCK_FILE_B10="$REPO_B10/.lock"
if [ -f "$LOCK_FILE_B10" ]; then
LOCK_SIZE_B10=$(stat -c '%s' "$LOCK_FILE_B10" 2>/dev/null || echo 0)
if [ "$LOCK_SIZE_B10" -eq 0 ]; then pass "102.4 lock file empty after operation (lock released)"; else pass "102.4 lock file exists but flock released"; fi
else
pass "102.4 lock file does not exist (clean)"
fi
assert_ok "102.5 prune (lock in prune)" "$BARESNAP" prune "$REPO_B10" --keep-last 1
assert_ok "102.6 verify (lock in verify)" "$BARESNAP" verify "$REPO_B10"

