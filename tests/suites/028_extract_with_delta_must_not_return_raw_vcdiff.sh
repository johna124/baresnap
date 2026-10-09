# ============================================================
# 28. EXTRACT with delta: must not return raw VCDIFF
# ============================================================
section "28. EXTRACT with delta: must not return raw VCDIFF"
REPO="$WORK/repo28"
SRC="$WORK/src28"
OUT="$WORK/out28"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "int main(int argc, char **argv) { return 0; } // BareSnap extract delta test" | head -c 65536 > "$SRC/code.c"
ORIG_SIZE=$(stat -c '%s' "$SRC/code.c")
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf '// MODIFIED LINE FOR DELTA
' >> "$SRC/code.c"
touch "$SRC/code.c"
assert_ok "create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT"
SRCBASE=$(basename "$SRC")
assert_ok "extract code.c from snap with delta" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT" "$SRCBASE/code.c"
EXTRACTED=$(find "$OUT" -name "code.c" -type f 2>/dev/null | head -1)
if [ -z "$EXTRACTED" ]; then
fail "extract did not produce code.c"
else
EXT_SIZE=$(stat -c '%s' "$EXTRACTED")
if [ "$EXT_SIZE" -lt "$ORIG_SIZE" ]; then fail "extract returned raw VCDIFF (${EXT_SIZE}B < ${ORIG_SIZE}B expected)"; else pass "extract returned full content (${EXT_SIZE}B)"; fi
FILETYPE=$(file -b "$EXTRACTED" 2>/dev/null)
if echo "$FILETYPE" | grep -qi "text"; then pass "extract output is valid text"; else fail "extract output is BINARY: $FILETYPE"; fi
if cmp -s "$SRC/code.c" "$EXTRACTED"; then pass "extract code.c byte-identical"; else fail "extract code.c is NOT byte-identical"; fi
fi

