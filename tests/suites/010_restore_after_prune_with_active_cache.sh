# ============================================================
# 10. Restore after prune with active cache
# ============================================================
section "10. Restore after prune with active cache"
REPO="$WORK/repo10"
SRC="$WORK/src10"
OUT="$WORK/out10"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'v1
' > "$SRC/f.txt"
assert_ok "create 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'v2
' > "$SRC/f.txt"
assert_ok "create 2" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
OUTC=$("$BARESNAP" create "$REPO" "$SRC" 2>&1)
HITSC=$(printf '%s
' "$OUTC" | grep -oE '[0-9]+ hits' | grep -oE '[0-9]+' | head -1)
assert_eq "create 3: cache hit after prune" "1" "$HITSC"
assert_ok "verify after prune + create" "$BARESNAP" verify "$REPO"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
RESTORED=$(cat "$BASE/f.txt")
assert_eq "content is v2" "v2" "$RESTORED"

