# ============================================================
# 88. Basic LIST: normal directory
# ============================================================
section "88. Basic LIST: normal directory"
REPO88="$WORK/repo88"
SRC88="$WORK/src88"
mkdir -p "$SRC88/subdir"
printf 'file1
' > "$SRC88/file1.txt"
printf 'file2
' > "$SRC88/file2.txt"
printf 'nested
' > "$SRC88/subdir/nested.txt"
assert_ok "88.1 init" "$BARESNAP" init "$REPO88"
assert_ok "88.2 create" "$BARESNAP" create "$REPO88" "$SRC88"
SNAP88=$(get_latest_snap "$REPO88")
LS_OUT88=$("$BARESNAP" ls "$REPO88" "$SNAP88" --recursive 2>/dev/null)
LS_RC88=$?
assert_eq "88.3 ls works" "0" "$LS_RC88"
if printf '%s
' "$LS_OUT88" | grep -q "file1\.txt"; then
pass "88.4 ls shows file1.txt"
else
fail "88.4 ls does not show file1.txt"
fi
if printf '%s
' "$LS_OUT88" | grep -q "subdir"; then
pass "88.5 ls shows subdir"
else
fail "88.5 ls does not show subdir"
fi
if printf '%s
' "$LS_OUT88" | grep -q "nested\.txt"; then
pass "88.6 ls shows nested.txt"
else
fail "88.6 ls does not show nested.txt"
fi

