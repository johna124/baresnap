# ============================================================
# 60. Network Chaos Monkey: intermittent SSH micro-cuts
# ============================================================
section "60. Network Chaos Monkey: intermittent SSH micro-cuts"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
CHAOS_TARGET="$(whoami)@localhost"
CHAOS_REMOTE="/tmp/baresnap_chaos_test"
CHAOS_URI="ssh://${CHAOS_TARGET}${CHAOS_REMOTE}"
CHAOS_SRC="$WORK/chaos_src"
ssh "$CHAOS_TARGET" "rm -rf $CHAOS_REMOTE" 2>/dev/null || true
mkdir -p "$CHAOS_SRC"
for i in $(seq 1 20); do
head -c 32768 /dev/urandom > "$CHAOS_SRC/file_$i.bin"
done
assert_ok "60.1 remote install" "$BARESNAP" remote install "$CHAOS_URI"
assert_ok "60.2 remote init" "$BARESNAP" init "$CHAOS_URI"
# Launch create in background with anti-deadlock timeout
timeout 120 "$BARESNAP" create "$CHAOS_URI" "$CHAOS_SRC" \
> "$WORK/chaos_create.log" 2>&1 &
CHAOS_PID=$!
CHAOS_APPLIED=0
SSH_CHILD=""
if command -v pgrep >/dev/null 2>&1; then
BRS_PID=""
# Find the baresnap child process of timeout
for _ in $(seq 1 30); do
BRS_PID=$(pgrep -P "$CHAOS_PID" -x "$(basename "$BARESNAP")" 2>/dev/null | head -n1 || true)
if [ -z "$BRS_PID" ]; then
BRS_PID=$(pgrep -P "$CHAOS_PID" -f "$BARESNAP" 2>/dev/null | head -n1 || true)
fi
[ -n "$BRS_PID" ] && break
sleep 0.1
done
# Find the ssh child process of baresnap
if [ -n "$BRS_PID" ]; then
for _ in $(seq 1 30); do
SSH_CHILD=$(pgrep -P "$BRS_PID" -x ssh 2>/dev/null | head -n1 || true)
if [ -z "$SSH_CHILD" ]; then
SSH_CHILD=$(pgrep -P "$BRS_PID" -f "ssh" 2>/dev/null | head -n1 || true)
fi
[ -n "$SSH_CHILD" ] && break
sleep 0.1
done
fi
fi
if [ -n "$SSH_CHILD" ]; then
pass "60.3 SSH process located (pid=$SSH_CHILD)"
CHAOS_APPLIED=1
# 5 micro-cut cycles: SIGSTOP 0.5s → SIGCONT 0.5s
for cycle in 1 2 3 4 5; do
if ! kill -0 "$CHAOS_PID" 2>/dev/null; then
break
fi
if kill -STOP "$SSH_CHILD" 2>/dev/null; then
sleep 0.5
kill -CONT "$SSH_CHILD" 2>/dev/null || true
sleep 0.5
else
break
fi
done
else
fail "60.3 child SSH not located; chaos not applied"
fi
# Wait for create to finish
wait "$CHAOS_PID" 2>/dev/null
CHAOS_RC=$?
if [ "$CHAOS_APPLIED" -eq 1 ]; then
if [ "$CHAOS_RC" -eq 124 ]; then
fail "60.4 DEADLOCK: create did not finish in 120s"
elif [ "$CHAOS_RC" -ge 128 ]; then
fail "60.4 create died by signal $((CHAOS_RC - 128)) during micro-cuts"
elif [ "$CHAOS_RC" -eq 0 ]; then
pass "60.4 create survived micro-cuts (rc=0)"
else
pass "60.4 create finished with controlled error (rc=$CHAOS_RC)"
fi
# Verify remote repo integrity
set +e
"$BARESNAP" verify "$CHAOS_URI" >/dev/null 2>&1
CHAOS_VERIFY=$?
set -e
if [ "$CHAOS_VERIFY" -eq 0 ]; then
pass "60.5 repo intact after chaos monkey"
else
fail "60.5 repo corrupt after chaos monkey"
fi
else
fail "60.4 create could not be subjected to micro-cuts"
fail "60.5 verify omitted because chaos not applied"
fi
ssh "$CHAOS_TARGET" "rm -rf $CHAOS_REMOTE" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi

