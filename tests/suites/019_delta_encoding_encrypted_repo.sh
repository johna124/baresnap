# ============================================================
# 19. Delta Encoding: encrypted repo
# ============================================================
section "19. Delta Encoding: encrypted repo"
REPO="$WORK/repo19"
SRC="$WORK/src19"
OUT="$WORK/out19"
BASE="$OUT/$(basename "$SRC")"
export BARESNAP_PASSPHRASE="delta-enc-test-pass"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
mkdir -p "$SRC"
yes "Encrypted delta encoding test content. Repeated for xdelta3 efficiency." | head -c 49152 > "$SRC/enc_delta.txt"
assert_ok "encrypted create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'SECRET' | dd of="$SRC/enc_delta.txt" bs=1 seek=2000 conv=notrunc status=none
touch "$SRC/enc_delta.txt"
assert_ok "encrypted create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
assert_ok "verify encrypted repo with delta" "$BARESNAP" verify "$REPO"
assert_ok "encrypted restore snapshot 2" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "enc_delta.txt byte-identical (encrypted+delta)" cmp -s "$SRC/enc_delta.txt" "$BASE/enc_delta.txt"
DIFF_OUT=$("$BARESNAP" diff "$REPO" "$SNAP1" "$SNAP2" 2>&1)
if printf '%s
' "$DIFF_OUT" | grep -q "M.*enc_delta\.txt"; then pass "encrypted diff detects modification"; else fail "encrypted diff does not detect modification"; fi
assert_ok "encrypted prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
assert_ok "encrypted verify after prune" "$BARESNAP" verify "$REPO"
rm -rf "$OUT"
assert_ok "restore after encrypted prune" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "enc_delta.txt correct after encrypted prune+restore" cmp -s "$SRC/enc_delta.txt" "$BASE/enc_delta.txt"
export BARESNAP_PASSPHRASE="wrong-pass"
set +e
"$BARESNAP" restore "$REPO" "$SNAP2" "$WORK/out_bad_delta" >/dev/null 2>&1
RC=$?
set -e
assert_eq "encrypted delta restore with wrong pass fails" "$RC" "1"
unset BARESNAP_PASSPHRASE

