# ============================================================
# 82. Entropy Firewall: Automatic ZSTD Bypass
# ============================================================
section "82. Entropy Firewall: Automatic ZSTD Bypass"
REPO82="$WORK/repo82"
SRC82="$WORK/src82"
assert_ok "82.1 init base repo" "$BARESNAP" init "$REPO82"
mkdir -p "$SRC82"
# 1. Create a highly compressible plain text file
for i in {1..2000}; do echo "BareSnap compressible log line text pattern $i"; done > "$SRC82/compressible.log"
# 2. Create a pure incompressible file (1 MB high entropy noise)
head -c 1048576 /dev/urandom > "$SRC82/high_entropy.mp4"
# Launch backup forcing ZSTD level 15 (To stress test)
assert_ok "82.2 create snapshot with mixed data in ZSTD 15" \
"$BARESNAP" create "$REPO82" "$SRC82" --compression zstd --zstd-level 15
# Extract statistics to verify the firewall via 'info'
INFO82=$("$BARESNAP" info "$REPO82" 2>&1)
# Storage audit: If the entropy bypass works,
# the chunk for 'high_entropy.mp4' must have been saved as UNCOMPRESSED.
# Therefore, the count of incompressible chunks must be greater than or equal to 1.
if printf '%s
' "$INFO82" | grep -q "uncompressed chunks:"; then
UNCOMP_COUNT=$(printf '%s
' "$INFO82" | grep "uncompressed chunks:" | grep -oE '[0-9]+' | head -1)
if [ "$UNCOMP_COUNT" -gt 0 ]; then
pass "82.3 Entropy firewall active ($UNCOMP_COUNT chunks skipped compression)"
else
fail "82.3 Firewall failed: 0 chunks skipped compression (CPU uselessly hammered)"
fi
else
fail "82.3 info does not show detailed chunk statistics"
fi

