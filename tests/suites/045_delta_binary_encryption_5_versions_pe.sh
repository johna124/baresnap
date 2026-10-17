# ============================================================
# 45. Delta binary + encryption: 5 versions (PE)
# ============================================================
section "45. Delta binary + encryption: 5 versions (PE)"
if ! "$BARESNAP" create --help 2>&1 | grep -q "delta-binary"; then
log "  [SKIP] --delta-binary not implemented"
else
REPO="$WORK/repo45"
SRC="$WORK/src45"
OUT="$WORK/out45"
BASE="$OUT/$(basename "$SRC")"
PE="$SRC/encrypted.exe"
export BARESNAP_PASSPHRASE="delta-binary-enc-test"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
mkdir -p "$SRC"
printf 'MZ\x90\x00\x03\x00\x00\x00\x04\x00\x00\x00\xff\xff\x00\x00' > "$PE"
head -c 524272 /dev/urandom >> "$PE"
assert_ok "encrypted create v1" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
cp "$PE" "$WORK/enc_v1.bin"
for v in 2 3 4 5; do
OFF1=$(( (v * 32768) % 523000 ))
head -c 128 /dev/urandom | dd of="$PE" bs=1 seek="$OFF1" conv=notrunc status=none 2>/dev/null
touch "$PE"
cp "$PE" "$WORK/enc_v${v}.bin"
sleep 1.1
assert_ok "encrypted create v$v" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
done
assert_ok "encrypted verify with delta binary" "$BARESNAP" verify "$REPO"
LAST_SNAP=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "encrypted restore v5" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "PE v5 byte-identical (encrypted+delta)" cmp -s "$WORK/enc_v5.bin" "$BASE/encrypted.exe"
assert_ok "encrypted prune --keep-last 2" "$BARESNAP" prune "$REPO" --keep-last 2
assert_ok "encrypted verify after prune" "$BARESNAP" verify "$REPO"
rm -rf "$OUT"
assert_ok "restore after encrypted prune" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "PE correct after encrypted prune+restore" cmp -s "$WORK/enc_v5.bin" "$BASE/encrypted.exe"
export BARESNAP_PASSPHRASE="wrong-pass"
set +e
"$BARESNAP" restore "$REPO" "$LAST_SNAP" "$WORK/out_bad_enc" >/dev/null 2>&1
RC=$?
set -e
assert_eq "encrypted delta restore with wrong pass fails" "$RC" "1"
unset BARESNAP_PASSPHRASE
PACKS_SIZE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
log "  [INFO] Encrypted + binary delta (5 versions): packs = ${PACKS_SIZE} KB"
fi

