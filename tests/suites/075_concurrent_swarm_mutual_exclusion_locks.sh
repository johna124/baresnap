# ============================================================
# 75. Concurrent Swarm: Mutual Exclusion Locks
# ============================================================
section "75. Concurrent Swarm: Mutual Exclusion"
REPO75="$WORK/repo75"
SRC75="$WORK/src75"
OUT75_DIR="$WORK/out75"
mkdir -p "$SRC75" "$OUT75_DIR"
assert_ok "75.1 init" "$BARESNAP" init "$REPO75"
echo "Concurrent burst" > "$SRC75/data.txt"
head -c 32768 /dev/urandom > "$SRC75/blob.bin"
declare -a PIDS75=()
declare -a RCS75=()
declare -a LOCKFAIL75=()
# Launch 5 requests surgically spaced to give time to the atomic lock
for i in 1 2 3 4 5; do
"$BARESNAP" create "$REPO75" "$SRC75" "snap_c$i" > "$OUT75_DIR/create_$i.txt" 2>&1 &
PIDS75[$i]=$!
RCS75[$i]=0
LOCKFAIL75[$i]=0
sleep 0.05
done
# Wait WITHOUT set -e killing the script
for i in 1 2 3 4 5; do
wait "${PIDS75[$i]}" 2>/dev/null || RCS75[$i]=$?
if grep -Eqi 'locked|lock file' "$OUT75_DIR/create_$i.txt" 2>/dev/null; then
LOCKFAIL75[$i]=1
fi
done
SUCCESS75=0
LOCKED75=0
UNEXPECTED75=0
for i in 1 2 3 4 5; do
if [ "${RCS75[$i]}" -eq 0 ]; then
SUCCESS75=$((SUCCESS75 + 1))
elif [ "${LOCKFAIL75[$i]}" -eq 1 ]; then
LOCKED75=$((LOCKED75 + 1))
else
UNEXPECTED75=$((UNEXPECTED75 + 1))
echo "--- create $i rc=${RCS75[$i]} ---" >&2
cat "$OUT75_DIR/create_$i.txt" >&2 || true
fi
done
if [ "$UNEXPECTED75" -gt 0 ]; then
fail "75.2 $UNEXPECTED75 process(es) failed for cause unrelated to lock"
else
pass "75.2 firing of 5 concurrent sub-processes completed (ok=$SUCCESS75 locked=$LOCKED75)"
fi
# The atomic lock must force the repository to remain intact
assert_ok "75.3 verify after concurrent burst" "$BARESNAP" verify "$REPO75"
# Count created snapshots (can be 1-5 depending on the lock)
SNAPS75=$(get_snap_count "$REPO75" 2>/dev/null || true)
SNAPS75=${SNAPS75:-0}
if ! [[ "$SNAPS75" =~ ^[0-9]+$ ]]; then
SNAPS75=0
fi
if [ "$SNAPS75" -ge 1 ]; then
pass "75.4 $SNAPS75 snapshot(s) created under concurrency"
else
fail "75.4 no snapshot created under concurrency (expected='>= 1' actual='$SNAPS75')"
fi
# Verify there are no temporaries or corruption
TMP75=0
if [ -d "$REPO75/tmp" ]; then
TMP75=$(find "$REPO75/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l || true)
fi
TMP75=${TMP75:-0}
if ! [[ "$TMP75" =~ ^[0-9]+$ ]]; then
TMP75=1
fi
if [ "$TMP75" -eq 0 ]; then
pass "75.5 no temporaries after concurrency"
else
fail "75.5 $TMP75 orphan temporaries after concurrency"
fi

