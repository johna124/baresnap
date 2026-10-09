# ============================================================
# 32. Type verification: restore and extract produce real text
# ============================================================
section "32. Type verification: restore and extract produce real text"
REPO="$WORK/repo32"
SRC="$WORK/src32"
OUT_R="$WORK/out32_r"
OUT_E="$WORK/out32_e"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
cat > "$SRC/real_code.c" << 'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
(void)argc;
(void)argv;
printf("hello from real code
");
return 0;
}
CEOF
for i in $(seq 1 500); do echo "// Line $i: padding for chunking test with delta encoding" >> "$SRC/real_code.c"; done
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf '// MODIFIED
' >> "$SRC/real_code.c"; touch "$SRC/real_code.c"
assert_ok "create snap 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT_R"
assert_ok "restore snap 2" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT_R"
R_FILE=$(find "$OUT_R" -name "real_code.c" -type f | head -1)
if [ -n "$R_FILE" ]; then
R_TYPE=$(file -b "$R_FILE")
if echo "$R_TYPE" | grep -qi "text"; then
R_HAS_INC=$(grep -c '#include' "$R_FILE")
R_HAS_MAIN=$(grep -c 'int main' "$R_FILE")
if [ "$R_HAS_INC" -gt 0 ] && [ "$R_HAS_MAIN" -gt 0 ]; then pass "restore: valid C text (#include=$R_HAS_INC, main=$R_HAS_MAIN)"; else fail "restore: text but without C markers"; fi
else
fail "restore: BINARY ($R_TYPE)"
fi
assert_ok "restore byte-identical" cmp -s "$SRC/real_code.c" "$R_FILE"
else
fail "restore did not produce real_code.c"
fi
rm -rf "$OUT_E"
assert_ok "extract snap 2" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT_E"
E_FILE=$(find "$OUT_E" -name "real_code.c" -type f | head -1)
if [ -n "$E_FILE" ]; then
E_TYPE=$(file -b "$E_FILE")
if echo "$E_TYPE" | grep -qi "text"; then
E_HAS_INC=$(grep -c '#include' "$E_FILE")
E_HAS_MAIN=$(grep -c 'int main' "$E_FILE")
if [ "$E_HAS_INC" -gt 0 ] && [ "$E_HAS_MAIN" -gt 0 ]; then pass "extract: valid C text (#include=$E_HAS_INC, main=$E_HAS_MAIN)"; else fail "extract: text but without C markers"; fi
else
fail "extract: BINARY ($E_TYPE)"
fi
assert_ok "extract byte-identical" cmp -s "$SRC/real_code.c" "$E_FILE"
else
fail "extract did not produce real_code.c"
fi

