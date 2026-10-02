# ============================================================
# 13. Diff between snapshots
# ============================================================
section "13. Diff between snapshots"
REPO="$WORK/repo13"
SRC="$WORK/src13"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'keep
' > "$SRC/keep.txt"
printf 'delete
' > "$SRC/delete_me.txt"
printf 'modify
' > "$SRC/modify.txt"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'modified
' > "$SRC/modify.txt"
printf 'added
' > "$SRC/added.txt"
rm "$SRC/delete_me.txt"
chmod 755 "$SRC/keep.txt"
assert_ok "create snapshot 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
DIFF_OUT="$("$BARESNAP" diff "$REPO" "$SNAP1" "$SNAP2" 2>&1)"
DIFF_RC=$?
assert_eq "diff exit code" "0" "$DIFF_RC"
if printf '%s
' "$DIFF_OUT" | grep -q "added\.txt"; then pass "diff detects added.txt"; else fail "diff does not detect added.txt"; fi
if printf '%s
' "$DIFF_OUT" | grep -q "delete_me\.txt"; then pass "diff detects delete_me.txt"; else fail "diff does not detect delete_me.txt"; fi
if printf '%s
' "$DIFF_OUT" | grep -q "modify\.txt"; then pass "diff detects modify.txt"; else fail "diff does not detect modify.txt"; fi
if printf '%s
' "$DIFF_OUT" | grep -q "keep\.txt"; then pass "diff detects keep.txt (metadata)"; else fail "diff does not detect keep.txt (metadata)"; fi
if printf '%s
' "$DIFF_OUT" | grep -q "unchanged:"; then pass "diff unchanged count correct"; else fail "diff unchanged count incorrect"; fi

