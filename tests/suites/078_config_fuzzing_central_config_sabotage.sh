# ============================================================
# 78. Config Fuzzing: central config sabotage
# ============================================================
section "78. Config Fuzzing: central config sabotage"
REPO78="$WORK/repo78"
SRC78="$WORK/src78"
assert_ok "78.1 init" "$BARESNAP" init "$REPO78"
mkdir -p "$SRC78"
printf 'fuzz
' > "$SRC78/f.txt"
# Destroy the config with binary garbage
dd if=/dev/urandom of="$REPO78/config" bs=1 count=64 conv=notrunc status=none 2>/dev/null
pass "78.2 config overwritten with garbage"
# list: must not crash (although it doesn't validate the config, it must not die by signal)
set +e
"$BARESNAP" list "$REPO78" >/dev/null 2>&1
LIST_RC78=$?
set -e
if [ "$LIST_RC78" -lt 128 ]; then
pass "78.3 list did not crash with corrupt config (rc=$LIST_RC78)"
else
fail "78.3 list CRASHED with signal $((LIST_RC78 - 128))"
fi
# verify: must reject the corrupt config
set +e
"$BARESNAP" verify "$REPO78" >/dev/null 2>&1
VERIFY_RC78=$?
set -e
if [ "$VERIFY_RC78" -ne 0 ]; then
pass "78.4 verify rejected corrupt config"
else
fail "78.4 verify accepted corrupt config"
fi
# create: must reject the corrupt config
set +e
"$BARESNAP" create "$REPO78" "$SRC78" >/dev/null 2>&1
CREATE_RC78=$?
set -e
if [ "$CREATE_RC78" -ne 0 ]; then
pass "78.5 create rejected corrupt config"
else
fail "78.5 create accepted corrupt config"
fi
# ============================================================
# 79 Battery Death: kill -9 during create
# ============================================================
section "79 Battery Death: kill -9 during create"
REPO79="$WORK/repo79"
SRC79="$WORK/src79"
assert_ok "79.1 init" "$BARESNAP" init "$REPO79"
mkdir -p "$SRC79"
# 128 MB distributed in 8 files of 16 MB + 200 small files
# to force enough metadata + I/O and that create lasts >1s
for i in $(seq 1 8); do
head -c 16777216 /dev/urandom > "$SRC79/heavy_$i.bin" 2>/dev/null
done
for i in $(seq 1 200); do
printf 'battery-death-file-%04d
' "$i" > "$SRC79/small_$i.txt"
done
pass "79.2 source created (128 MB + 200 small files)"
# Launch create in background
"$BARESNAP" create "$REPO79" "$SRC79" </dev/null >/dev/null 2>&1 &
PID79=$!
# Polling loop: wait for the process to be really active
# and then kill it. Maximum 5 seconds of waiting.
KILLED79=0
for attempt in $(seq 1 100); do
sleep 0.05
if ! kill -0 "$PID79" 2>/dev/null; then
# The process already finished before being able to kill it
break
fi
# The process is still alive: kill it with kill -9
kill -9 "$PID79" 2>/dev/null
wait "$PID79" 2>/dev/null || true
KILLED79=1
break
done
if [ "$KILLED79" -eq 1 ]; then
pass "79.3 kill -9 sent in the middle of create (pid=$PID79)"
else
wait "$PID79" 2>/dev/null || true
fail "79.3 create finished before kill (test not applied)"
fi
if [ "$KILLED79" -eq 1 ]; then
# health --repair must clean temporaries and leave the repo intact
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO79" --repair </dev/null >/dev/null 2>&1 || true
TMP79=$(find "$REPO79/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP79" -gt 0 ]; then
fail "79.4 $TMP79 zombie temporaries after kill -9"
else
pass "79.4 no temporaries after kill -9"
fi
set +e
"$BARESNAP" verify "$REPO79" >/dev/null 2>&1
VERIFY_RC79=$?
set -e
if [ "$VERIFY_RC79" -eq 0 ]; then
pass "79.5 verify OK after kill -9 + repair"
else
# If no snapshot was created (very early kill), verify can
# pass trivially or fail due to empty repo. Both are valid.
SNAPS79=$(get_snap_count "$REPO79")
if [ "$SNAPS79" -eq 0 ]; then
pass "79.5 verify OK (empty repo after premature kill)"
else
fail "79.5 verify fails after kill -9 + repair (rc=$VERIFY_RC79)"
fi
fi
else
fail "79.4 skip (kill not applied)"
fail "79.5 skip (kill not applied)"
fi

