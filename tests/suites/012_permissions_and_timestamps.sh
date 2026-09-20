# ============================================================
# 12. Permissions and timestamps
# ============================================================
section "12. Permissions and timestamps"
REPO="$WORK/repo12"
SRC="$WORK/src12"
OUT="$WORK/out12"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'perm test
' > "$SRC/perm.txt"
chmod 0604 "$SRC/perm.txt"
touch -d '2020-01-15 10:30:00 UTC' "$SRC/perm.txt"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
MODE_SRC=$(stat -c '%a' "$SRC/perm.txt")
MODE_OUT=$(stat -c '%a' "$BASE/perm.txt")
assert_eq "permissions preserved (0604)" "$MODE_SRC" "$MODE_OUT"
MTIME_SRC=$(stat -c '%Y' "$SRC/perm.txt")
MTIME_OUT=$(stat -c '%Y' "$BASE/perm.txt")
assert_eq "mtime preserved" "$MTIME_SRC" "$MTIME_OUT"

