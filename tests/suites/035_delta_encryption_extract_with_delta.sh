# ============================================================
# 35. Delta + encryption: extract with delta
# ============================================================
section "35. Delta + encryption: extract with delta"
REPO="$WORK/repo35"
SRC="$WORK/src35"
OUT="$WORK/out35"
export BARESNAP_PASSPHRASE="extract-delta-enc-test"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
mkdir -p "$SRC"
yes "ENCRYPTED DELTA EXTRACT TEST content for xdelta3" | head -c 49152 > "$SRC/enc_delta.txt"
assert_ok "encrypted create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'ENC_MOD
' >> "$SRC/enc_delta.txt"; touch "$SRC/enc_delta.txt"
assert_ok "encrypted create snap 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "encrypted extract with delta" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT"
E_FILE=$(find "$OUT" -name "enc_delta.txt" -type f | head -1)
if [ -n "$E_FILE" ]; then
assert_ok "enc_delta.txt byte-identical (encrypted+delta+extract)" cmp -s "$SRC/enc_delta.txt" "$E_FILE"
E_TYPE=$(file -b "$E_FILE")
if echo "$E_TYPE" | grep -qi "text"; then pass "encrypted extract: valid text"; else fail "encrypted extract: BINARY ($E_TYPE)"; fi
else
fail "encrypted extract did not produce enc_delta.txt"
fi
unset BARESNAP_PASSPHRASE

