# ============================================================
# 52. CRC32C INTEGRITY (Pack corruption)
# ============================================================
section "52. CRC32C INTEGRITY (corruption detection)"
REPO52="$WORK/repo52"
SRC52="$WORK/src52"
BACKUP52="$WORK/pack_backups52"
mkdir -p "$SRC52" "$BACKUP52"
# Create test data
echo "test data for CRC32C verification" > "$SRC52/testfile.txt"
head -c 4096 /dev/urandom > "$SRC52/random.bin"
assert_ok "52.1 init" "$BARESNAP" init "$REPO52"
assert_ok "52.2 create" "$BARESNAP" create "$REPO52" "$SRC52"
# Verify it passes with clean packs
set +e
"$BARESNAP" verify "$REPO52" >/dev/null 2>&1
VERIFY_CLEAN52=$?
set -e
if [ "$VERIFY_CLEAN52" -eq 0 ]; then
pass "52.3 verify OK with clean packs"
else
fail "52.3 verify fails with clean packs"
fi
# Locate packs and backup
mapfile -t PACKS52 < <(find "$REPO52/packs" -maxdepth 1 -name '*.pack' -type f)
if [ ${#PACKS52[@]} -gt 0 ]; then
for p in "${PACKS52[@]}"; do
cp "$p" "$BACKUP52/$(basename "$p")"
done
pass "52.4 backup of ${#PACKS52[@]} pack(s) performed"
# Corrupt the first pack (1 byte at offset 100)
FIRST_PACK52="${PACKS52[0]}"
printf '\xFF' | dd of="$FIRST_PACK52" bs=1 seek=100 count=1 conv=notrunc status=none 2>/dev/null
pass "52.5 pack corrupted (1 byte at offset 100)"
# Verify it detects corruption
set +e
"$BARESNAP" verify "$REPO52" >/dev/null 2>&1
VERIFY_CORRUPT52=$?
set -e
if [ "$VERIFY_CORRUPT52" -ne 0 ]; then
pass "52.6 verify detects CRC32C corruption (rc=$VERIFY_CORRUPT52)"
else
fail "52.6 verify DID NOT detect CRC32C corruption"
fi
# Restore clean pack
for p in "${PACKS52[@]}"; do
cp "$BACKUP52/$(basename "$p")" "$p"
done
# Verify it passes again
set +e
"$BARESNAP" verify "$REPO52" >/dev/null 2>&1
VERIFY_RESTORE52=$?
set -e
if [ "$VERIFY_RESTORE52" -eq 0 ]; then
pass "52.7 verify OK after restoring clean pack"
else
fail "52.7 verify fails after restoring clean pack"
fi
else
fail "52.4 no packs found"
fail "52.5 skip"
fail "52.6 skip"
fail "52.7 skip"
fi

