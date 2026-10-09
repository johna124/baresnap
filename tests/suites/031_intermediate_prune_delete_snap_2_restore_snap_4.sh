# ============================================================
# 31. Intermediate prune: delete snap 2, restore snap 4
# ============================================================
section "31. Intermediate prune: delete snap 2, restore snap 4"
REPO="$WORK/repo31"
SRC="$WORK/src31"
OUT="$WORK/out31"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "PRUNE CHAIN TEST. Repeated content for delta encoding across snapshots." | head -c 65536 > "$SRC/prunechain.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'P2
' >> "$SRC/prunechain.txt"; touch "$SRC/prunechain.txt"
assert_ok "create snap 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
sleep 1.1
printf 'P3
' >> "$SRC/prunechain.txt"; touch "$SRC/prunechain.txt"
assert_ok "create snap 3" "$BARESNAP" create "$REPO" "$SRC"
SNAP3=$(get_latest_snap "$REPO")
sleep 1.1
printf 'P4
' >> "$SRC/prunechain.txt"; touch "$SRC/prunechain.txt"
assert_ok "create snap 4" "$BARESNAP" create "$REPO" "$SRC"
SNAP4=$(get_latest_snap "$REPO")
assert_ok "prune --keep-last 2" "$BARESNAP" prune "$REPO" --keep-last 2
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "2 snapshots after prune" "2" "$SNAPS_AFTER"
assert_ok "verify after intermediate prune" "$BARESNAP" verify "$REPO"
rm -rf "$OUT"
assert_ok "restore snap 4 after prune" "$BARESNAP" restore "$REPO" "$SNAP4" "$OUT"
assert_ok "prunechain.txt correct after prune+restore" cmp -s "$SRC/prunechain.txt" "$BASE/prunechain.txt"
rm -rf "$OUT"
assert_ok "extract snap 4 after prune" "$BARESNAP" extract "$REPO" "$SNAP4" "$OUT"
if [ -f "$BASE/prunechain.txt" ]; then
assert_ok "prunechain.txt correct after prune+extract" cmp -s "$SRC/prunechain.txt" "$BASE/prunechain.txt"
else
fail "extract did not produce prunechain.txt after prune"
fi

