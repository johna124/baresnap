# ============================================================
# 22. Delta: verification of real savings in packs
# ============================================================
section "22. Delta: space savings in packs"
REPO="$WORK/repo22"
SRC="$WORK/src22"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "SAVINGS TEST. This line repeats for delta efficiency testing." | head -c 131072 > "$SRC/savings.txt"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SIZE_BEFORE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
sleep 1.1
printf 'ZZZ' | dd of="$SRC/savings.txt" bs=1 seek=1000 conv=notrunc status=none
touch "$SRC/savings.txt"
assert_ok "create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SIZE_AFTER=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
GROWTH=$((SIZE_AFTER - SIZE_BEFORE))
if [ "$GROWTH" -le 32 ]; then
pass "delta savings: growth ${GROWTH} KB <= 32 KB (128 KB file)"
else
fail "delta savings: growth ${GROWTH} KB > 32 KB (128 KB file)"
fi
assert_ok "verify after delta savings" "$BARESNAP" verify "$REPO"

