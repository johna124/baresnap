# ============================================================
# 24. Index corruption (.idx) — FNV1A-64 footer
# ============================================================
section "24. Index corruption (.idx)"
REPO="$WORK/repo24"
SRC="$WORK/src24"
OUT="$WORK/out24"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'index integrity test
' > "$SRC/data.txt"
head -c 131072 /dev/urandom > "$SRC/blob.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before corrupting" "$BARESNAP" verify "$REPO"
IDX_FILE=$(ls "$REPO/index/"*.idx 2>/dev/null | head -1)
IDX_BACKUP="$WORK/idx_backup.idx"
SNAP_NAME=$(get_latest_snap "$REPO")
if [ -n "$IDX_FILE" ]; then
cp "$IDX_FILE" "$IDX_BACKUP"
IDX_SIZE=$(wc -c < "$IDX_FILE")
CORRUPT_OFFSET=$((IDX_SIZE / 2))
printf '\xDE' | dd of="$IDX_FILE" bs=1 seek="$CORRUPT_OFFSET" conv=notrunc status=none
set +e
"$BARESNAP" verify "$REPO" >/dev/null 2>&1
VERIFY_RC=$?
set -e
if [ "$VERIFY_RC" -ne 0 ]; then pass "verify detects corrupt index (rc=$VERIFY_RC)"; else fail "verify does not detect corrupt index"; fi
set +e
"$BARESNAP" info "$REPO" >/dev/null 2>&1
INFO_RC=$?
set -e
assert_eq "info does not abort with corrupt index" "0" "$INFO_RC"
cp "$IDX_BACKUP" "$IDX_FILE"
assert_ok "verify passes after restoring index" "$BARESNAP" verify "$REPO"
assert_ok "restore works after restoring index" "$BARESNAP" restore "$REPO" "$SNAP_NAME" "$OUT"
BASE_OUT="$OUT/$(basename "$SRC")"
assert_ok "correct content after restore" cmp -s "$SRC/blob.bin" "$BASE_OUT/blob.bin"
else
fail "no index found to corrupt"
fi

