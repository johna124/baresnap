# ============================================================
# 77. Pack Tamper: physical mutation in encrypted pack (AES-256-GCM)
# ============================================================
section "77. Pack Tamper: physical mutation in encrypted pack (AES-256-GCM)"
REPO77="$WORK/repo77"
SRC77="$WORK/src77"
export BARESNAP_PASSPHRASE="tamper-pass-77"
assert_init_aes "77.1 init aes" "$BARESNAP" init "$REPO77" --encrypt aes
assert_info_aes "77.1b info confirms AES" "$BARESNAP" info "$REPO77"
mkdir -p "$SRC77"
printf 'data for tamper
' > "$SRC77/tamper.txt"
head -c 131072 /dev/urandom > "$SRC77/blob.bin"
assert_ok "77.2 create aes" "$BARESNAP" create "$REPO77" "$SRC77"
assert_ok "77.3 verify before tamper" "$BARESNAP" verify "$REPO77"
PACK77=$(find "$REPO77/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | head -1)
if [ -n "$PACK77" ]; then
PACK77_SIZE=$(stat -c '%s' "$PACK77")
OFF77=$((PACK77_SIZE / 2))
printf '\xFF' | dd of="$PACK77" bs=1 seek="$OFF77" conv=notrunc status=none 2>/dev/null
pass "77.4 byte injected at offset $OFF77 (encrypted data zone)"
set +e
"$BARESNAP" verify "$REPO77" >/dev/null 2>&1
VERIFY_RC77=$?
set -e
if [ "$VERIFY_RC77" -ne 0 ]; then
pass "77.5 verify detected corrupt pack (GCM/CRC)"
else
fail "77.5 verify did NOT detect the mutation"
fi
else
fail "77.4 no pack found to mutate"
fail "77.5 skip"
fi
unset BARESNAP_PASSPHRASE

