# ============================================================
# 20. Delta: corruption detection/source deletion
# ============================================================
section "20. Delta: deleted pack detection (delta source)"
REPO="$WORK/repo20"
SRC="$WORK/src20"
OUT="$WORK/out20"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "CORRUPTION TEST BASE. Repeated for delta encoding." | head -c 32768 > "$SRC/corr.txt"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'XXX' | dd of="$SRC/corr.txt" bs=1 seek=100 conv=notrunc status=none
touch "$SRC/corr.txt"
assert_ok "create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
assert_ok "verify before deleting pack" "$BARESNAP" verify "$REPO"
PACK=$(ls "$REPO/packs/"*.pack 2>/dev/null | head -1)
if [ -n "$PACK" ]; then
rm "$PACK"
if "$BARESNAP" verify "$REPO" >/dev/null 2>&1; then
fail "verify should detect deleted pack (delta source)"
else
pass "verify detects deleted pack (delta source)"
fi
else
fail "no pack found to delete"
fi

