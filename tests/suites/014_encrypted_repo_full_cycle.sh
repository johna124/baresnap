# ============================================================
# 14. Encrypted repo: full cycle
# ============================================================
section "14. Encrypted repo: full cycle"
REPO="$WORK/repo14"
SRC="$WORK/src14"
OUT="$WORK/out14"
BASE="$OUT/$(basename "$SRC")"
mkdir -p "$SRC/subdir"
printf 'secret content
' > "$SRC/secret.txt"
printf 'nested secret
' > "$SRC/subdir/nested.txt"
dd if=/dev/urandom of="$SRC/big.bin" bs=1M count=2 2>/dev/null
export BARESNAP_PASSPHRASE="test-passphrase-123"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
assert_ok "encrypted create" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "list without passphrase" "$BARESNAP" list "$REPO"
SNAP=$(get_latest_snap "$REPO")
assert_ok "encrypted restore" "$BARESNAP" restore "$REPO" "$SNAP" "$OUT"
assert_ok "secret.txt content" test "$(cat "$BASE/secret.txt")" = "secret content"
assert_ok "nested.txt content" test "$(cat "$BASE/subdir/nested.txt")" = "nested secret"
assert_ok "big.bin byte-identical" cmp -s "$SRC/big.bin" "$BASE/big.bin"
assert_ok "encrypted verify" "$BARESNAP" verify "$REPO"
FOUND=0
if grep -rq "secret content" "$REPO/packs/" 2>/dev/null; then FOUND=1; fi
assert_eq "packs without plaintext" "$FOUND" "0"
FOUND=0
if grep -rq "secret.txt\|secret content" "$REPO/snapshots/" 2>/dev/null; then FOUND=1; fi
assert_eq "snapshots without plaintext" "$FOUND" "0"
MAGIC=$(head -c 7 "$REPO/snapshots/$SNAP")
assert_eq "snapshot magic is BRSNAP2" "BRSNAP2" "$MAGIC"
export BARESNAP_PASSPHRASE="wrong-passphrase"
set +e
"$BARESNAP" restore "$REPO" "$SNAP" "$WORK/out_bad" >/dev/null 2>&1
assert_eq "restore with wrong passphrase fails" "$?" "1"
"$BARESNAP" verify "$REPO" >/dev/null 2>&1
assert_eq "verify with wrong passphrase fails" "$?" "1"
set -e
unset BARESNAP_PASSPHRASE

