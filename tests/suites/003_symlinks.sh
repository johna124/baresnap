# ============================================================
# 03. Symlinks
# ============================================================
section "03. Symlinks"
REPO="$WORK/repo03"
SRC="$WORK/src03"
OUT="$WORK/out03"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'target content
' > "$SRC/target.txt"
if ln -s target.txt "$SRC/link.txt" 2>/dev/null; then
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
if [ -L "$BASE/link.txt" ]; then
LINK_TARGET="$(readlink "$BASE/link.txt")"
assert_eq "restored symlink points to target.txt" "target.txt" "$LINK_TARGET"
assert_ok "content via symlink" cmp -s "$SRC/target.txt" "$BASE/target.txt"
else
fail "link.txt is not a symlink after restore"
fi
else
fail "could not create symlink in test filesystem"
fi

