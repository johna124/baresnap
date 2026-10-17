# ============================================================
# 08. Corruption detection
# ============================================================
section "08. Corruption detection"
REPO="$WORK/repo08"
SRC="$WORK/src08"
OUT="$WORK/out08"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 131072 /dev/urandom > "$SRC/data.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before corrupting" "$BARESNAP" verify "$REPO"
PACK=$(ls "$REPO/packs/"*.pack 2>/dev/null | head -1)
if [ -n "$PACK" ]; then
printf '\xFF' | dd of="$PACK" bs=1 seek=1000 conv=notrunc status=none
if "$BARESNAP" verify "$REPO" >/dev/null 2>&1; then
fail "verify should detect corruption"
else
pass "verify detects corruption (exit != 0)"
fi
else
fail "no pack found to corrupt"
fi

