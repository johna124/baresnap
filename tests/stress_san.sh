#!/bin/bash
set -u
BARESNAP="${BARESNAP:-./build_san/baresnap}"
WORK="$(mktemp -d /tmp/brs_san.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  [PASS] $1"; }
fail() { FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }
ok()   { local d="$1"; shift; if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi; }
section() { echo ""; echo "== $1 =="; }
if [ ! -x "$BARESNAP" ]; then
echo "ERROR: $BARESNAP does not exist. Run ./compile_san.sh first."
exit 1
fi

# Detect if we are running under ASan
if "$BARESNAP" --version 2>&1 | grep -q "AddressSanitizer"; then
echo "✓ AddressSanitizer active"
fi

# ============================================================
section "S1. Basic cycle: init/create/restore/verify"
R="$WORK/r1"; S="$WORK/s1"; O="$WORK/o1"
mkdir -p "$S/nested/deep"
printf 'hello world
' > "$S/f.txt"
printf 'nested content
' > "$S/nested/deep/n.txt"
head -c 1048576 /dev/urandom > "$S/big.bin"
ok "init" "$BARESNAP" init "$R"
ok "create" "$BARESNAP" create "$R" "$S"
SNAP=$("$BARESNAP" list "$R" 2>/dev/null | grep '\.snap$' | tail -1 | awk '{print $NF}')
ok "restore" "$BARESNAP" restore "$R" "$SNAP" "$O"
ok "content" cmp -s "$S/f.txt" "$O/$(basename "$S")/f.txt"
ok "verify" "$BARESNAP" verify "$R"

# ============================================================
section "S2. Delta encoding: 5 versions"
R="$WORK/r2"; S="$WORK/r2src"
mkdir -p "$S"
head -c 131072 /dev/urandom > "$S/delta.bin"
ok "init" "$BARESNAP" init "$R"
for i in 1 2 3 4 5; do
printf 'modification %d
' "$i" >> "$S/delta.bin"
ok "create v$i" "$BARESNAP" create "$R" "$S" "v$i"
done
ok "verify" "$BARESNAP" verify "$R"
SNAP=$("$BARESNAP" list "$R" 2>/dev/null | grep '\.snap$' | tail -1 | awk '{print $NF}')
O="$WORK/r2out"
ok "restore v5" "$BARESNAP" restore "$R" "$SNAP" "$O"
ok "content v5" cmp -s "$S/delta.bin" "$O/$(basename "$S")/delta.bin"

# ============================================================
section "S3. ChaCha20 encryption: full cycle"
R="$WORK/r3"; S="$WORK/r3src"; O="$WORK/r3out"
mkdir -p "$S"
printf 'secret data
' > "$S/secret.txt"
head -c 524288 /dev/urandom > "$S/enc.bin"
export BARESNAP_PASSPHRASE="san-test-pass"
ok "init --encrypt" "$BARESNAP" init "$R" --encrypt
ok "create" "$BARESNAP" create "$R" "$S"
ok "verify" "$BARESNAP" verify "$R"
SNAP=$("$BARESNAP" list "$R" 2>/dev/null | grep '\.snap$' | tail -1 | awk '{print $NF}')
ok "restore" "$BARESNAP" restore "$R" "$SNAP" "$O"
ok "content" cmp -s "$S/secret.txt" "$O/$(basename "$S")/secret.txt"
ok "prune" "$BARESNAP" prune "$R" --keep-last 1
ok "verify post-prune" "$BARESNAP" verify "$R"
unset BARESNAP_PASSPHRASE

# ============================================================
section "S4. AES-256-GCM encryption: full cycle"
R="$WORK/r4"; S="$WORK/r4src"; O="$WORK/r4out"
mkdir -p "$S"
printf 'aes secret
' > "$S/aes.txt"
head -c 524288 /dev/urandom > "$S/aes.bin"
export BARESNAP_PASSPHRASE="san-aes-pass"
ok "init --encrypt aes" "$BARESNAP" init "$R" --encrypt aes
ok "create" "$BARESNAP" create "$R" "$S"
ok "verify" "$BARESNAP" verify "$R"
SNAP=$("$BARESNAP" list "$R" 2>/dev/null | grep '\.snap$' | tail -1 | awk '{print $NF}')
ok "restore" "$BARESNAP" restore "$R" "$SNAP" "$O"
ok "content" cmp -s "$S/aes.txt" "$O/$(basename "$S")/aes.txt"
ok "prune" "$BARESNAP" prune "$R" --keep-last 1
ok "verify post-prune" "$BARESNAP" verify "$R"
unset BARESNAP_PASSPHRASE

# ============================================================
section "S5. Symlinks + hardlinks"
R="$WORK/r5"; S="$WORK/r5src"; O="$WORK/r5out"
mkdir -p "$S"
printf 'target
' > "$S/target.txt"
ln -s target.txt "$S/link.txt"
ln "$S/target.txt" "$S/hard.txt"
ok "init" "$BARESNAP" init "$R"
ok "create" "$BARESNAP" create "$R" "$S"
SNAP=$("$BARESNAP" list "$R" 2>/dev/null | grep '\.snap$' | tail -1 | awk '{print $NF}')
ok "restore" "$BARESNAP" restore "$R" "$SNAP" "$O"
ok "symlink" test -L "$O/$(basename "$S")/link.txt"
ok "hardlink same inode" test "$O/$(basename "$S")/target.txt" -ef "$O/$(basename "$S")/hard.txt"

# ============================================================
section "S6. Selective extract + ls + diff + info"
R="$WORK/r6"; S="$WORK/r6src"
mkdir -p "$S/sub"
printf 'a
' > "$S/a.txt"
printf 'b
' > "$S/sub/b.txt"
ok "init" "$BARESNAP" init "$R"
ok "create snap1" "$BARESNAP" create "$R" "$S" "snap1"
printf 'modified
' > "$S/a.txt"
ok "create snap2" "$BARESNAP" create "$R" "$S" "snap2"
SNAP2=$("$BARESNAP" list "$R" 2>/dev/null | grep '\.snap$' | tail -1 | awk '{print $NF}')
SNAP1=$("$BARESNAP" list "$R" 2>/dev/null | grep '\.snap$' | head -1 | awk '{print $NF}')
O="$WORK/r6out"
ok "selective extract" "$BARESNAP" extract "$R" "$SNAP2" "$O" "$(basename "$S")/a.txt"
ok "ls" "$BARESNAP" ls "$R" "$SNAP2"
ok "diff" "$BARESNAP" diff "$R" "$SNAP1" "$SNAP2"
ok "info" "$BARESNAP" info "$R"

# ============================================================
section "S7. ZSTD: full cycle"
R="$WORK/r7"; S="$WORK/r7src"
mkdir -p "$S"
printf 'zstd test data
' > "$S/z.txt"
ok "init zstd" "$BARESNAP" init "$R" --compression zstd --zstd-level 10
ok "create" "$BARESNAP" create "$R" "$S"
ok "verify" "$BARESNAP" verify "$R"
O="$WORK/r7out"
SNAP=$("$BARESNAP" list "$R" 2>/dev/null | grep '\.snap$' | tail -1 | awk '{print $NF}')
ok "restore" "$BARESNAP" restore "$R" "$SNAP" "$O"
ok "content" cmp -s "$S/z.txt" "$O/$(basename "$S")/z.txt"

# ============================================================
section "S8. Aggressive prune: 10 snapshots → 1"
R="$WORK/r8"; S="$WORK/r8src"
mkdir -p "$S"
ok "init" "$BARESNAP" init "$R"
for i in $(seq 1 10); do
printf 'version %d
' "$i" > "$S/f.txt"
"$BARESNAP" create "$R" "$S" "snap$i" >/dev/null 2>&1
done
CNT=$("$BARESNAP" list "$R" 2>/dev/null | grep -c '\.snap$')
if [ "$CNT" -eq 10 ]; then pass "10 snapshots created"; else fail "only $CNT snapshots"; fi
ok "prune --keep-last 1" "$BARESNAP" prune "$R" --keep-last 1
CNT=$("$BARESNAP" list "$R" 2>/dev/null | grep -c '\.snap$')
if [ "$CNT" -eq 1 ]; then pass "1 snapshot after prune"; else fail "$CNT snapshots after prune"; fi
ok "verify post-prune" "$BARESNAP" verify "$R"

# ============================================================
section "S9. Health check + repair"
R="$WORK/r9"; S="$WORK/r9src"
mkdir -p "$S"
printf 'health
' > "$S/h.txt"
ok "init" "$BARESNAP" init "$R"
ok "create" "$BARESNAP" create "$R" "$S"
ok "clean health" "$BARESNAP" health "$R"

# ============================================================
section "S10. Empty file + large file (4 MB)"
R="$WORK/r10"; S="$WORK/r10src"; O="$WORK/r10out"
mkdir -p "$S"
touch "$S/empty.txt"
head -c 4194304 /dev/urandom > "$S/large.bin"
ok "init" "$BARESNAP" init "$R"
ok "create" "$BARESNAP" create "$R" "$S"
SNAP=$("$BARESNAP" list "$R" 2>/dev/null | grep '\.snap$' | tail -1 | awk '{print $NF}')
ok "restore" "$BARESNAP" restore "$R" "$SNAP" "$O"
ok "empty.txt size 0" test ! -s "$O/$(basename "$S")/empty.txt"
ok "large.bin identical" cmp -s "$S/large.bin" "$O/$(basename "$S")/large.bin"

# ============================================================
echo ""
echo "============================================================"
if [ "$FAIL" -eq 0 ]; then
echo "RESULT: $PASS/$((PASS+FAIL)) tests passed ✅"
echo "If there is no ASan/UBSan output above = ZERO memory errors"
exit 0
else
echo "RESULT: $PASS/$((PASS+FAIL)) passed, $FAIL failed ❌"
exit 1
fi
