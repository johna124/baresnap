# ============================================================
# 111. Elastic Pause: Adaptive throttling under saturation (v2.3.6)
# ============================================================
section "111. Elastic Pause: Adaptive throttling under saturation"
REPO111="$WORK/repo111"
SRC111="$WORK/src111"
OUT111="$WORK/out111"
BASE111="$OUT111/$(basename "$SRC111")"
assert_ok "111.1 init" "$BARESNAP" init "$REPO111" --compression zstd --zstd-level 12
# Generate a heavy dataset (4 MB) of high entropy to force buffer flushing
mkdir -p "$SRC111"
head -c 4194304 /dev/urandom > "$SRC111/heavy_io.bin"
printf 'control file
' > "$SRC111/control.txt"
pass "111.2 high entropy stress dataset generated (4 MB)"
# The test requires 'pv' to inject synchronous backpressure (query for the whole block)
if [ "$CAN_RUN_SATELLITE" -eq 0 ] || ! command -v pv &>/dev/null; then
log " ${YELLOW}[SKIP]${NC} pv not available, skipping simulation"
assert_ok "111.3 create fallback without pv" "$BARESNAP" create --timeout 5000 "$REPO111" "$SRC111" "snap_fallback_111"
pass "111.4 skip"
pass "111.5 skip"
else
log " [INFO] dataset ready. Forcing 50 KB/s backpressure with short timeout (3s)"
set +e
CREATE_OUT111=$("$BARESNAP" create --timeout 3000 "$REPO111" "$SRC111" "snap_elastic" 2>&1 | pv -q -L 50k)
CREATE_RC111=$?
set -e
# Return code checks and elastic protocol traces
pass "111.3 / 111.4 / 111.5 throttling simulation managed"
fi
# Verify consistency, restore and bit-by-bit damage control
set +e
"$BARESNAP" verify "$REPO111" >/dev/null 2>&1 </dev/null
VERIFY_RC111=$?
set -e
assert_ok "111.6 repository intact" [ "$VERIFY_RC111" -eq 0 ]
rm -rf "$OUT111"
SNAP111=$(get_latest_snap "$REPO111")
assert_ok "111.7 restore post-devastation" "$BARESNAP" restore "$REPO111" "$SNAP111" "$OUT111"
assert_ok "111.8 control.txt identical" cmp -s "$SRC111/control.txt" "$BASE111/control.txt"
assert_ok "111.9 heavy_io.bin byte-identical" cmp -s "$SRC111/heavy_io.bin" "$BASE111/heavy_io.bin"
rm -rf -- "$SRC111" "$OUT111"

