# ============================================================
# 61. Ghost Permission: chmod 000 during create
# ============================================================
section "61. Ghost Permission: chmod 000 during create"
REPO61="$WORK/repo61"
SRC61="$WORK/src61"
assert_ok "61.1 init" "$BARESNAP" init "$REPO61"
mkdir -p "$SRC61"
# Create 500 small files so create takes long enough
for i in $(seq 1 500); do
printf 'perm ghost file %d
' "$i" > "$SRC61/file_$i.txt"
done
# Launch create in background
"$BARESNAP" create "$REPO61" "$SRC61" \
> "$WORK/perm_create.log" 2>&1 &
PERM_PID=$!
# Wait 20ms and remove read permissions from half the files
sleep 0.02
CHMOD_COUNT=0
for i in $(seq 1 500); do
if [ $((i % 2)) -eq 0 ]; then
chmod 000 "$SRC61/file_$i.txt" 2>/dev/null || true
CHMOD_COUNT=$((CHMOD_COUNT + 1))
fi
done
# Wait for create to finish
PERM_RC=0
wait "$PERM_PID" 2>/dev/null || PERM_RC=$?
# Restore permissions for cleanup
chmod -R u+rw "$SRC61" 2>/dev/null || true
if [ "$PERM_RC" -ge 128 ]; then
fail "61.2 create CRASHED with signal $((PERM_RC - 128))"
elif [ "$PERM_RC" -eq 0 ]; then
pass "61.2 create completed with $CHMOD_COUNT chmod 000 files"
else
pass "61.2 create finished with controlled error (rc=$PERM_RC, $CHMOD_COUNT chmod 000)"
fi
# Verify that the repo is not corrupt
set +e
"$BARESNAP" verify "$REPO61" >/dev/null 2>&1
PERM_VERIFY=$?
set -e
if [ "$PERM_VERIFY" -eq 0 ]; then
pass "61.3 repo intact after ghost chmod"
else
fail "61.3 repo corrupt after ghost chmod"
fi
# Verify there are no orphan temporary packs
TMP61=$(find "$REPO61/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP61" -eq 0 ]; then
pass "61.4 no orphan temporaries in tmp/"
else
fail "61.4 $TMP61 orphan temporaries in tmp/"
fi
# Verify that the snapshot was created (even if partial)
SNAP61=$(get_snap_count "$REPO61")
if [ "$SNAP61" -ge 1 ]; then
pass "61.5 snapshot created ($SNAP61) despite chmod 000"
else
fail "61.5 no snapshot was created"
fi

