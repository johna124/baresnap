# ============================================================
# 50. EXTREME CONCURRENCY (Append-Only)
# ============================================================
section "50. Extreme concurrency (5 simultaneous processes)"
REPO50="$WORK/repo50"
SRC50="$WORK/src50"
NUM_PROCS=5
assert_ok "50.1 init" "$BARESNAP" init "$REPO50"
# Create 5 data sources
mkdir -p "$SRC50"
for i in $(seq 1 $NUM_PROCS); do
mkdir -p "$SRC50/src_$i"
head -c 102400 /dev/urandom > "$SRC50/src_$i/data_$i.bin"
done
pass "50.2 sources created ($NUM_PROCS x 100KB)"
# Launch concurrent processes
PIDS50=()
for i in $(seq 1 $NUM_PROCS); do
(
set +e
"$BARESNAP" create "$REPO50" "$SRC50/src_$i" > "$WORK/output50_$i.txt" 2>&1
RC=$?
set -e
echo "$RC" > "$WORK/rc50_$i.txt"
) &
PIDS50+=($!)
done
# Wait for all
for pid in "${PIDS50[@]}"; do
wait $pid 2>/dev/null || true
done
pass "50.3 all processes finished"
# Count successes
SUCCESS50=0
for i in $(seq 1 $NUM_PROCS); do
if [ -f "$WORK/rc50_$i.txt" ]; then
RC=$(cat "$WORK/rc50_$i.txt" | tr -d '[:space:]')
if [ "$RC" -eq 0 ]; then
SUCCESS50=$((SUCCESS50 + 1))
fi
fi
done
if [ "$SUCCESS50" -eq "$NUM_PROCS" ]; then
pass "50.4 all processes succeeded ($SUCCESS50/$NUM_PROCS)"
elif [ "$SUCCESS50" -ge 1 ]; then
pass "50.4 some processes succeeded ($SUCCESS50/$NUM_PROCS)"
else
fail "50.4 no process succeeded"
fi
# Verify created snapshots
SNAP50=$(ls "$REPO50/snapshots/"*.snap 2>/dev/null | wc -l)
if [ "$SNAP50" -ge 1 ]; then
pass "50.5 $SNAP50 snapshots created"
else
fail "50.5 no snapshots created"
fi
# Verify integrity
set +e
"$BARESNAP" verify "$REPO50" >/dev/null 2>&1
VERIFY_RC50=$?
set -e
if [ "$VERIFY_RC50" -eq 0 ]; then
pass "50.6 repo intact after concurrency"
else
fail "50.6 repo corrupt after concurrency"
fi

