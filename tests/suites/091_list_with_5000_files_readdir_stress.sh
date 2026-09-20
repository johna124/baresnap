# ============================================================
# 91. LIST with 5000 files (readdir stress)
# ============================================================
section "91. LIST with 5000 files (readdir stress)"
REPO91="$WORK/repo91"
SRC91="$WORK/src91"
rm -rf "$REPO91" "$SRC91"
mkdir -p "$SRC91"
for i in $(seq 1 5000); do
printf 'data_%05d
' "$i" > "$SRC91/file_$(printf '%05d' $i).txt"
done
pass "91.1 5000 files created"
assert_ok "91.2 init" "$BARESNAP" init "$REPO91"
assert_ok "91.3 create with 5000 files" "$BARESNAP" create "$REPO91" "$SRC91"
SNAP91=$(get_latest_snap "$REPO91")
LS_OUT91=$("$BARESNAP" ls "$REPO91" "$SNAP91" --recursive 2>/dev/null)
LS_RC91=$?
assert_eq "91.4 ls with 5000 files works" "0" "$LS_RC91"
LS_COUNT91=$(printf '%s
' "$LS_OUT91" | awk '/file_/{n++} END{print n+0}')
if [ "$LS_COUNT91" -ge 4990 ]; then
pass "91.5 ls lists $LS_COUNT91/5000 files"
else
fail "91.5 ls only lists $LS_COUNT91/5000 files"
fi
OUT91="$WORK/out91"
SRCBASE91=$(basename "$SRC91")
assert_ok "91.6 selective extract of 5000" "$BARESNAP" extract "$REPO91" "$SNAP91" "$OUT91" "$SRCBASE91/file_02500.txt"
EXTRACTED91=$(find "$OUT91" -name "file_02500.txt" -type f 2>/dev/null | head -1)
if [ -n "$EXTRACTED91" ] && grep -q "data_02500" "$EXTRACTED91"; then
pass "91.7 selective extract correct"
else
fail "91.7 selective extract incorrect"
fi
assert_ok "91.8 verify with 5000 files" "$BARESNAP" verify "$REPO91"

