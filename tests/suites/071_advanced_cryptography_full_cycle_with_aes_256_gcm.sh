# ============================================================
# 71. Advanced Cryptography: Full Cycle with AES-256-GCM
# ============================================================
section "71. Advanced Cryptography: Full Cycle with AES-256-GCM"
# Verify that the binary supports --encrypt aes
if ! "$BARESNAP" init --help 2>&1 | grep -qi "aes"; then
log "  ${YELLOW}[SKIP]${NC} --encrypt aes not implemented"
else
REPO71="$WORK/repo71"
SRC71="$WORK/src71"
OUT71="$WORK/out71"
BASE71="$OUT71/$(basename "$SRC71")"
rm -rf "$REPO71" "$SRC71" "$OUT71" "$WORK/out71_bad"
mkdir -p "$SRC71"
printf 'Highly confidential data protected by hardware
' > "$SRC71/top_secret.db"
head -c 512000 /dev/urandom > "$SRC71/crypto_test.bin"
export BARESNAP_PASSPHRASE="ClaveEspartanaConAES256"
AES71_OK=1


if ! assert_init_aes "71.1 init with --encrypt aes --compression zstd" \
"$BARESNAP" init "$REPO71" --encrypt aes --compression zstd --zstd-level 5; then
    AES71_OK=0
fi


if [ "$AES71_OK" -eq 1 ] && ! assert_info_aes "71.1b info confirms AES" \
"$BARESNAP" info "$REPO71"; then
AES71_OK=0
fi
if [ "$AES71_OK" -eq 1 ]; then
assert_ok "71.2 create with AES-256-GCM pipeline" \
"$BARESNAP" create "$REPO71" "$SRC71" "snap_hardware_crypto"
# Opacity audit: zero plaintext in packs/snapshots
FOUND_PLAIN=0
if grep -rq "confidenciales" "$REPO71/packs/" 2>/dev/null; then
FOUND_PLAIN=1
fi
if grep -rq "confidenciales" "$REPO71/snapshots/" 2>/dev/null; then
FOUND_PLAIN=1
fi
assert_eq "71.3 zero plaintext leaks in physical structures" "0" "$FOUND_PLAIN"
# Cross integrity verification (CRC32C + GCM tag + hash ID)
assert_ok "71.4 verify: GCM authentication and integrity correct" \
"$BARESNAP" verify "$REPO71"
# Resilience against incorrect passphrase
export BARESNAP_PASSPHRASE="ContrasenaIncorrecta999"
set +e
"$BARESNAP" restore "$REPO71" "$(get_latest_snap "$REPO71")" "$WORK/out71_bad" >/dev/null 2>&1
RC71_BAD=$?
set +e
assert_eq "71.5 cryptographic firewall blocks false passphrase" "1" "$RC71_BAD"
# Successful bit-by-bit restore with correct passphrase
export BARESNAP_PASSPHRASE="ClaveEspartanaConAES256"
SNAP71=$(get_latest_snap "$REPO71")
rm -rf "$OUT71"
assert_ok "71.6 legitimate restore with AES-256-GCM" \
"$BARESNAP" restore "$REPO71" "$SNAP71" "$OUT71"
assert_ok "71.6 top_secret.db byte-identical" \
cmp -s "$SRC71/top_secret.db" "$BASE71/top_secret.db"
assert_ok "71.6 crypto_test.bin byte-identical" \
cmp -s "$SRC71/crypto_test.bin" "$BASE71/crypto_test.bin"
# Prune + verify with AES
assert_ok "71.7 prune --keep-last 1 with AES-256-GCM" \
"$BARESNAP" prune "$REPO71" --keep-last 1
assert_ok "71.7 verify after prune with AES" \
"$BARESNAP" verify "$REPO71"
else
fail "71.2 create with AES-256-GCM pipeline (skip due to init/info AES failure)"
fail "71.3 zero plaintext leaks in physical structures (skip)"
fail "71.4 verify: GCM authentication and integrity correct (skip)"
fail "71.5 cryptographic firewall blocks false passphrase (skip)"
fail "71.6 legitimate restore with AES-256-GCM (skip)"
fail "71.7 prune --keep-last 1 with AES-256-GCM (skip)"
fi
unset BARESNAP_PASSPHRASE
fi

