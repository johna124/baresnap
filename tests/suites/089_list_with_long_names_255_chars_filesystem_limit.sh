# ============================================================
# 89. LIST with long names (255 chars, filesystem limit)
# ============================================================
section "89. LIST with long names (255 chars)"
REPO89="$WORK/repo89"
SRC89="$WORK/src89"
OUT89="$WORK/out89"
mkdir -p "$SRC89"
LONG_NAME89=$(printf 'A%.0s' $(seq 1 250))
LONG_NAME89="${LONG_NAME89}.txt"
printf 'long name content
' > "$SRC89/$LONG_NAME89"
assert_ok "89.1 init" "$BARESNAP" init "$REPO89"
assert_ok "89.2 create with long name" "$BARESNAP" create "$REPO89" "$SRC89"
SNAP89=$(get_latest_snap "$REPO89")
LS_OUT89=$("$BARESNAP" ls "$REPO89" "$SNAP89" --recursive 2>/dev/null)
LS_RC89=$?
assert_eq "89.3 ls with long name works" "0" "$LS_RC89"
if printf '%s
' "$LS_OUT89" | grep -q "AAAA"; then
pass "89.4 ls shows file with long name"
else
fail "89.4 ls does not show file with long name"
fi
assert_ok "89.5 restore with long name" "$BARESNAP" restore "$REPO89" "$SNAP89" "$OUT89"
BASE89="$OUT89/$(basename "$SRC89")"
if [ -f "$BASE89/$LONG_NAME89" ]; then
pass "89.6 long file restored correctly"
else
fail "89.6 long file NOT restored"
fi

