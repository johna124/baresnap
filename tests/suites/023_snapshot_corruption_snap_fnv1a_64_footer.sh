# ============================================================
# 23. Snapshot corruption (.snap) — FNV1A-64 footer
# ============================================================
section "23. Snapshot corruption (.snap)"
REPO="$WORK/repo23"
SRC="$WORK/src23"
OUT="$WORK/out23"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'snapshot integrity test
' > "$SRC/data.txt"
head -c 65536 /dev/urandom > "$SRC/blob.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before corrupting" "$BARESNAP" verify "$REPO"
SNAP_FILE=$(ls "$REPO/snapshots/"*.snap 2>/dev/null | head -1)
SNAP_BACKUP="$WORK/snap_backup.snap"
if [ -n "$SNAP_FILE" ]; then
cp "$SNAP_FILE" "$SNAP_BACKUP"
SNAP_SIZE=$(wc -c < "$SNAP_FILE")
CORRUPT_OFFSET=$((SNAP_SIZE / 2))
printf '\xFF' | dd of="$SNAP_FILE" bs=1 seek="$CORRUPT_OFFSET" conv=notrunc status=none
set +e
"$BARESNAP" verify "$REPO" >/dev/null 2>&1
VERIFY_RC=$?
set -e
if [ "$VERIFY_RC" -ne 0 ]; then pass "verify detects corrupt snapshot (rc=$VERIFY_RC)"; else fail "verify does not detect corrupt snapshot"; fi
set +e
"$BARESNAP" restore "$REPO" "$(basename "$SNAP_FILE")" "$OUT" >/dev/null 2>&1
RESTORE_RC=$?
set -e
if [ "$RESTORE_RC" -ne 0 ]; then pass "restore fails with corrupt snapshot (rc=$RESTORE_RC)"; else fail "restore does not fail with corrupt snapshot"; fi
cp "$SNAP_BACKUP" "$SNAP_FILE"
assert_ok "verify passes after restoring snapshot" "$BARESNAP" verify "$REPO"
assert_ok "restore works after restoring snapshot" "$BARESNAP" restore "$REPO" "$(basename "$SNAP_FILE")" "$OUT"
else
fail "no snapshot found to corrupt"
fi

