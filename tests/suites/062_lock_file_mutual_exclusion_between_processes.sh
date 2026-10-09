# ============================================================
# 62. Lock File: mutual exclusion between processes
# ============================================================
section "62. Lock File: mutual exclusion between processes"
REPO62="$WORK/repo62"
SRC62="$WORK/src62"
OUT62_1="$WORK/lock_out1.txt"
OUT62_2="$WORK/lock_out2.txt"
assert_ok "62.1 init" "$BARESNAP" init "$REPO62"
mkdir -p "$SRC62"
# Enough data so create takes time and the lock activates
for i in $(seq 1 20); do
head -c 65536 /dev/urandom > "$SRC62/file_$i.bin"
done
assert_ok "62.2 initial create" "$BARESNAP" create "$REPO62" "$SRC62"
# Modify data to force a second real create
for i in $(seq 1 20); do
printf 'MOD' | dd of="$SRC62/file_$i.bin" bs=1 seek=100 conv=notrunc status=none 2>/dev/null
done
RC1=0
RC2=0
# Launch two simultaneous creates.
# IMPORTANT: `wait` must go with `|| RC=$?` if the suite uses `set -e`.
"$BARESNAP" create "$REPO62" "$SRC62" > "$OUT62_1" 2>&1 &
PID1=$!
sleep 0.2
"$BARESNAP" create "$REPO62" "$SRC62" > "$OUT62_2" 2>&1 &
PID2=$!
wait "$PID1" 2>/dev/null || RC1=$?
wait "$PID2" 2>/dev/null || RC2=$?
LOCK_FAIL1=0
LOCK_FAIL2=0
if grep -Eqi 'locked|lock file' "$OUT62_1" 2>/dev/null; then
LOCK_FAIL1=1
fi
if grep -Eqi 'locked|lock file' "$OUT62_2" 2>/dev/null; then
LOCK_FAIL2=1
fi
if [ "$RC1" -eq 0 ] && [ "$RC2" -eq 0 ]; then
pass "62.3 both creates completed (did not overlap or lock serialized by timing)"
elif [ "$RC1" -eq 0 ] && [ "$RC2" -ne 0 ] && [ "$LOCK_FAIL2" -eq 1 ]; then
pass "62.3 create 1 completed; create 2 rejected by lock"
elif [ "$RC2" -eq 0 ] && [ "$RC1" -ne 0 ] && [ "$LOCK_FAIL1" -eq 1 ]; then
pass "62.3 create 2 completed; create 1 rejected by lock"
else
echo "--- $OUT62_1 ---" >&2
cat "$OUT62_1" >&2 || true
echo "--- $OUT62_2 ---" >&2
cat "$OUT62_2" >&2 || true
fail "62.3 unexpected result (rc1=$RC1, rc2=$RC2, lock1=$LOCK_FAIL1, lock2=$LOCK_FAIL2)"
fi
# Verify that the lock did not leave temporaries or corrupt the repo
TMP62=0
if [ -d "$REPO62/tmp" ]; then
TMP62=$(find "$REPO62/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l || true)
fi
TMP62=${TMP62:-1}
if [ "$TMP62" -eq 0 ]; then
pass "62.4 no temporaries after concurrency with lock"
else
fail "62.4 $TMP62 orphan temporaries after lock"
fi
assert_ok "62.5 verify after concurrency with lock" "$BARESNAP" verify "$REPO62"
SNAPS62=$(get_snap_count "$REPO62")
if [ "$SNAPS62" -ge 2 ]; then
pass "62.6 snapshots created correctly ($SNAPS62)"
else
fail "62.6 only $SNAPS62 snapshots (expected >= 2)"
fi

