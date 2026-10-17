# ============================================================
# 57. Broken Mirror: bit rot injection in pack
# ============================================================
section "57. Broken Mirror: bit rot injection in pack"
REPO57="$WORK/repo57"
SRC57="$WORK/src57"
assert_ok "57.1 init" "$BARESNAP" init "$REPO57"
mkdir -p "$SRC57"
head -c 262144 /dev/urandom > "$SRC57/bitrot.bin"
assert_ok "57.2 create" "$BARESNAP" create "$REPO57" "$SRC57"
assert_ok "57.3 verify before corrupting" "$BARESNAP" verify "$REPO57"
# Locate the pack
PACK57=$(find "$REPO57/packs" -maxdepth 1 -name '*.pack' -type f -print -quit 2>/dev/null)
if [ -n "$PACK57" ]; then
PACK_SIZE57=$(stat -c '%s' "$PACK57")
MID57=$((PACK_SIZE57 / 2))
# Inject corrupt byte in the middle of the pack data area
printf '\xDE' | dd of="$PACK57" bs=1 seek="$MID57" conv=notrunc status=none 2>/dev/null
pass "57.4 corrupt byte injected at offset $MID57"
# Verify must fail with integrity error (CRC32C or hash)
set +e
VERIFY_OUT57=$("$BARESNAP" verify "$REPO57" 2>&1)
VERIFY_RC57=$?
set -e
if [ "$VERIFY_RC57" -ne 0 ]; then
if echo "$VERIFY_OUT57" | grep -qiE "hash|crc|corrupt|mismatch|checksum|decompress"; then
pass "57.5 verify detects bit rot with integrity message"
else
pass "57.5 verify detects bit rot (rc=$VERIFY_RC57)"
fi
else
fail "57.5 verify did NOT detect injected bit rot"
fi
# Restore the pack and verify that health detects it as corrupt
printf '\xDE' | dd of="$PACK57" bs=1 seek="$MID57" conv=notrunc status=none 2>/dev/null
HEALTH_RC57=0
HEALTH_OUT57=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO57" </dev/null 2>&1) || HEALTH_RC57=$?
if [ "$HEALTH_RC57" -ge 1 ]; then
pass "57.6 health detects corrupt pack (rc=$HEALTH_RC57)"
else
fail "57.6 health does not detect corrupt pack"
fi
else
fail "57.4 no pack found to inject bit rot"
fail "57.5 skip"
fail "57.6 skip"
fi

