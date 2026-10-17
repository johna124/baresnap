# ============================================================
# 47. Accidental deletion and sector errors
# ============================================================
section "47. Accidental deletion and sector errors"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
# ------------------------------------------------------------
# 47.1 Corrupt sector inside a pack
# ------------------------------------------------------------
REPO="$WORK/repo47_sector"
SRC="$WORK/src47_sector"
OUT="$WORK/out47_sector"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init (corrupt sector)" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 131072 /dev/urandom > "$SRC/data.bin"
assert_ok "create (corrupt sector)" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before corrupting sector" "$BARESNAP" verify "$REPO"
PACK=$(find "$REPO/packs" -maxdepth 1 -type f -name '*.pack' -print -quit 2>/dev/null)
PACK_BACKUP="$WORK/repo47_sector_pack.backup"
if [ -n "$PACK" ]; then
cp "$PACK" "$PACK_BACKUP"
PACK_SIZE=$(stat -c '%s' "$PACK")
if [ "$PACK_SIZE" -gt 8192 ]; then
SECTOR=4096
OFF=$((PACK_SIZE / 2))
if [ "$OFF" -gt $((PACK_SIZE - SECTOR)) ]; then
OFF=$((PACK_SIZE - SECTOR))
fi
head -c "$SECTOR" /dev/urandom | dd of="$PACK" bs=1 seek="$OFF" conv=notrunc status=none 2>/dev/null
else
OFF=$((PACK_SIZE / 2))
[ "$OFF" -lt 16 ] && OFF=16
head -c 16 /dev/urandom | dd of="$PACK" bs=1 seek="$OFF" conv=notrunc status=none 2>/dev/null
fi
VERIFY_RC=0
"$BARESNAP" verify "$REPO" >/dev/null 2>&1 </dev/null || VERIFY_RC=$?
if [ "$VERIFY_RC" -ne 0 ]; then
pass "verify detects corrupt sector in pack"
else
fail "verify did not detect corrupt sector in pack"
fi
SNAP=$(get_latest_snap "$REPO")
RESTORE_RC=0
"$BARESNAP" restore "$REPO" "$SNAP" "$OUT" >/dev/null 2>&1 </dev/null || RESTORE_RC=$?
if [ "$RESTORE_RC" -ne 0 ] || ! cmp -s "$SRC/data.bin" "$BASE/data.bin"; then
pass "restore does not recover valid data with corrupt sector"
else
fail "restore returned valid data with corrupt sector"
fi
cp "$PACK_BACKUP" "$PACK"
assert_ok "verify passes after restoring healthy pack" "$BARESNAP" verify "$REPO"
else
fail "no pack found to simulate corrupt sector"
fi
# ------------------------------------------------------------
# 47.2 Accidental deletion of pack
# ------------------------------------------------------------
REPO="$WORK/repo47_borrado"
SRC="$WORK/src47_borrado"
assert_ok "init (accidental deletion)" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 131072 /dev/urandom > "$SRC/data.bin"
assert_ok "create (accidental deletion)" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before deleting pack" "$BARESNAP" verify "$REPO"
PACK=$(find "$REPO/packs" -maxdepth 1 -type f -name '*.pack' -print -quit 2>/dev/null)
if [ -n "$PACK" ]; then
rm -f -- "$PACK"
VERIFY_RC=0
"$BARESNAP" verify "$REPO" >/dev/null 2>&1 </dev/null || VERIFY_RC=$?
if [ "$VERIFY_RC" -ne 0 ]; then
pass "verify detects accidentally deleted pack"
else
fail "verify did not detect deleted pack"
fi
HEALTH_OUT=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" </dev/null 2>&1) || true
if printf '%s
' "$HEALTH_OUT" | grep -Eq 'sin pack|chunk'; then
pass "health detects anomalies due to deleted pack"
else
fail "health did not detect anomalies due to deleted pack"
fi
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" --repair </dev/null >/dev/null 2>&1 || true
DAMAGED_COUNT=$(find "$REPO/damaged" -type f -name '*.snap' 2>/dev/null | wc -l)
if [ "$DAMAGED_COUNT" -ge 1 ]; then
pass "repair moved damaged snapshot to damaged/ ($DAMAGED_COUNT)"
else
fail "repair did not move damaged snapshot to damaged/"
fi
LIVE_SNAPS=$(get_snap_count "$REPO")
assert_eq "damaged snapshots no longer appear as active" "0" "$LIVE_SNAPS"
assert_ok "verify after repair (isolated repo)" "$BARESNAP" verify "$REPO"
else
fail "no pack found to simulate accidental deletion"
fi
fi

