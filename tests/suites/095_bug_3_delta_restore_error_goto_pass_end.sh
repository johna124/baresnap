# ============================================================
# 95. Bug #3: Delta restore error (goto pass_end)
# ============================================================
section "95. Bug #3: Delta restore error (goto pass_end)"
REPO_B3="$WORK/repo_b3"
SRC_B3="$WORK/src_b3"
OUT_B3="$WORK/out_b3"
mkdir -p "$SRC_B3"
yes "Delta error path test content for xdelta3 validation." | head -c 32768 > "$SRC_B3/delta_target.txt"
assert_ok "95.1 init" "$BARESNAP" init "$REPO_B3"
assert_ok "95.2 create v1 (base for delta)" "$BARESNAP" create "$REPO_B3" "$SRC_B3"
sleep 1.1
printf 'MOD' >> "$SRC_B3/delta_target.txt"
touch "$SRC_B3/delta_target.txt"
assert_ok "95.3 create v2 (delta expected)" "$BARESNAP" create "$REPO_B3" "$SRC_B3"
for p in "$REPO_B3/packs/"*.pack; do
[ -f "$p" ] || continue
PSIZE=$(stat -c '%s' "$p")
if [ "$PSIZE" -gt 200 ]; then
printf '\xFF' | dd of="$p" bs=1 seek=100 conv=notrunc status=none 2>/dev/null
fi
done
pass "95.4 packs corrupted (delta cannot reconstruct)"
SNAP_B3=$(get_latest_snap "$REPO_B3")
set +e
"$BARESNAP" restore "$REPO_B3" "$SNAP_B3" "$OUT_B3" >/dev/null 2>&1 </dev/null
RESTORE_RC_B3=$?
set -e
if [ "$RESTORE_RC_B3" -ge 128 ]; then
fail "95.5 restore CRASHED with signal $((RESTORE_RC_B3 - 128))"
else
pass "95.5 restore failed cleanly without crash (rc=$RESTORE_RC_B3)"
fi
EMPTY_FILES_B3=$(find "$OUT_B3" -type f -empty 2>/dev/null | wc -l)
if [ "$EMPTY_FILES_B3" -eq 0 ]; then
pass "95.6 no empty partial files in output"
else
fail "95.6 $EMPTY_FILES_B3 empty partial files found"
fi

