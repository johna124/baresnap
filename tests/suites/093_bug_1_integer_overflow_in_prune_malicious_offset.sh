# ============================================================
# 93. Bug #1: Integer overflow in prune (malicious offset)
# ============================================================
section "93. Bug #1: Integer overflow in prune (malicious offset)"
REPO_B1="$WORK/repo_b1"
SRC_B1="$WORK/src_b1"
mkdir -p "$SRC_B1"
printf 'overflow test data
' > "$SRC_B1/data.txt"
head -c 32768 /dev/urandom > "$SRC_B1/blob.bin"
assert_ok "93.1 init" "$BARESNAP" init "$REPO_B1"
assert_ok "93.2 create" "$BARESNAP" create "$REPO_B1" "$SRC_B1"
PACK_B1=$(find "$REPO_B1/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | head -1)
if [ -n "$PACK_B1" ]; then
PACK_SIZE_B1=$(stat -c '%s' "$PACK_B1")
FOOTER_START=$((PACK_SIZE_B1 - 28 - 33))
if [ "$FOOTER_START" -gt 36 ]; then
OFFSET_POS=$((FOOTER_START + 8 + 8 + 4 + 16))
printf '\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFE' | \
dd of="$PACK_B1" bs=1 seek="$OFFSET_POS" conv=notrunc status=none 2>/dev/null
pass "93.3 malicious offset injected in pack"
else
fail "93.3 pack too small to inject offset"
fi
else
fail "93.3 no pack found to corrupt"
fi
set +e
"$BARESNAP" prune "$REPO_B1" --keep-last 1 >/dev/null 2>&1 </dev/null
PRUNE_RC_B1=$?
set -e
if [ "$PRUNE_RC_B1" -ge 128 ]; then
fail "93.4 prune CRASHED with signal $((PRUNE_RC_B1 - 128)) (OOB read likely)"
else
pass "93.4 prune did not crash with malicious offset (rc=$PRUNE_RC_B1)"
fi
set +e
"$BARESNAP" verify "$REPO_B1" >/dev/null 2>&1 </dev/null
VERIFY_RC_B1=$?
set -e
if [ "$VERIFY_RC_B1" -ge 128 ]; then
fail "93.5 verify CRASHED with signal $((VERIFY_RC_B1 - 128))"
else
pass "93.5 verify did not crash with malicious offset (rc=$VERIFY_RC_B1)"
fi

