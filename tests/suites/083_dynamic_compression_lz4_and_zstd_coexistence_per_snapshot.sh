# ============================================================
# 83. Dynamic Compression: LZ4 and ZSTD Coexistence per Snapshot
# ============================================================
section "83. Dynamic Compression: LZ4 and ZSTD Coexistence per Snapshot"
REPO83="$WORK/repo83"
SRC83_1="$WORK/src83_1"
SRC83_2="$WORK/src83_2"
mkdir -p "$SRC83_1" "$SRC83_2"
# Initialize a repo with LZ4 by default
assert_ok "83.1 init repo with LZ4 by default" "$BARESNAP" init "$REPO83" --compression lz4
# Snapshot 1: Dynamically force ZSTD level 12 for heavy logs
for i in {1..1000}; do echo "Log pattern heavy compression session $i"; done > "$SRC83_1/logs.txt"
assert_ok "83.2 create snapshot 1 forcing dynamic ZSTD" \
"$BARESNAP" create "$REPO83" "$SRC83_1" --compression zstd --zstd-level 12
# Snapshot 2: Dynamically force LZ4 for lightweight code files
echo "int main() { return 0; }" > "$SRC83_2/main.c"
assert_ok "83.3 create snapshot 2 forcing dynamic LZ4" \
"$BARESNAP" create "$REPO83" "$SRC83_2" --compression lz4
# Validate that info can audit the mixed repository
INFO83=$("$BARESNAP" info "$REPO83" 2>&1)
if printf '%s
' "$INFO83" | grep -q "compressed chunks:"; then
pass "83.4 Info correctly reads repository with mixed dynamic compression"
else
fail "83.4 Info failed to analyze mixed structure"
fi
# Validate absolute integrity of the transactional database with multiple algorithms
assert_ok "83.5 global verify on mixed LZ4/ZSTD repository successful" "$BARESNAP" verify "$REPO83"

