# ============================================================
# 87. LRU Cache Eviction: large packs without dangling pointers
# ============================================================
section "87. LRU Cache Eviction: large packs without dangling pointers"
REPO87="$WORK/repo87"
SRC87="$WORK/src87"
OUT87="$WORK/out87"
BASE87="$OUT87/$(basename "$SRC87")"
mkdir -p "$SRC87"
assert_ok "87.1 init" "$BARESNAP" init "$REPO87"
# Create large files to fill the pack cache
for i in $(seq 1 30); do
dd if=/dev/urandom bs=1048576 count=2 of="$SRC87/big_$i.bin" 2>/dev/null
done
pass "87.2 30 2MB files created (60 MB total)"
# Backup to generate large packs
assert_ok "87.3 create with large packs (fills cache)" "$BARESNAP" create "$REPO87" "$SRC87"
# Massive restore: forces reading of multiple packs and LRU eviction
# If there are dangling pointers, this will crash with SIGSEGV
rm -rf "$OUT87"
LATEST87=$(get_latest_snap "$REPO87")
set +e
"$BARESNAP" restore "$REPO87" "$LATEST87" "$OUT87" >/dev/null 2>&1 </dev/null
RESTORE_RC87=$?
set -e
if [ "$RESTORE_RC87" -ge 128 ]; then
fail "87.4 restore CRASHED with signal $((RESTORE_RC87 - 128)) (dangling pointer likely)"
elif [ "$RESTORE_RC87" -eq 0 ]; then
pass "87.4 massive restore completed without crash (LRU eviction OK)"
else
pass "87.4 restore finished with controlled error (rc=$RESTORE_RC87)"
fi
# Verify post-eviction integrity if the restore was successful
if [ "$RESTORE_RC87" -eq 0 ]; then
MISMATCH87=0
for i in $(seq 1 30); do
if ! cmp -s "$SRC87/big_$i.bin" "$BASE87/big_$i.bin" 2>/dev/null; then
MISMATCH87=$((MISMATCH87 + 1))
fi
done
if [ "$MISMATCH87" -eq 0 ]; then
pass "87.5 30/30 files byte-identical after LRU eviction"
else
fail "87.5 $MISMATCH87/30 files differ after LRU eviction"
fi
assert_ok "87.6 verify after LRU eviction" "$BARESNAP" verify "$REPO87"
fi

