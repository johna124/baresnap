# ============================================================
# 46. Delta binary: prune + restore in deep chain (ELF)
# ============================================================
section "46. Delta binary: prune + restore in deep chain (ELF)"
if ! "$BARESNAP" create --help 2>&1 | grep -q "delta-binary"; then
log "  [SKIP] --delta-binary not implemented"
else
REPO="$WORK/repo46"
SRC="$WORK/src46"
OUT="$WORK/out46"
BASE="$OUT/$(basename "$SRC")"
ELF="$SRC/deep.elf"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "$ELF"
head -c 524276 /dev/urandom >> "$ELF"
assert_ok "create v1" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
for v in $(seq 2 10); do
OFF1=$(( (v * 32768) % 523000 ))
head -c 64 /dev/urandom | dd of="$ELF" bs=1 seek="$OFF1" conv=notrunc status=none 2>/dev/null
touch "$ELF"
sleep 1.1
assert_ok "create v$v" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
done
SNAPS_BEFORE=$(get_snap_count "$REPO")
assert_eq "10 snapshots before prune" "10" "$SNAPS_BEFORE"
cp "$ELF" "$WORK/deep_v10_final.bin"
assert_ok "prune --keep-last 3" "$BARESNAP" prune "$REPO" --keep-last 3
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "3 snapshots after prune" "3" "$SNAPS_AFTER"
assert_ok "verify after delta chain prune" "$BARESNAP" verify "$REPO"
LAST_SNAP=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore v10 after prune" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "deep.elf v10 correct after prune" cmp -s "$WORK/deep_v10_final.bin" "$BASE/deep.elf"
PACKS_SIZE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
log "  [INFO] After prune --keep-last 3 (of 10 versions): packs = ${PACKS_SIZE} KB"
if [ "$PACKS_SIZE" -lt 2000 ]; then
pass "prune + delta: compact repository (${PACKS_SIZE} KB)"
else
fail "prune + delta: packs too large (${PACKS_SIZE} KB)"
fi
fi

