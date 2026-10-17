# ============================================================
# 42. Delta binary: ELF 10 versions with --delta-binary
# ============================================================
section "42. Delta binary: ELF 10 versions with --delta-binary"
if ! "$BARESNAP" create --help 2>&1 | grep -q "delta-binary"; then
log "  [SKIP] --delta-binary not implemented"
else
REPO="$WORK/repo42"
SRC="$WORK/src42"
OUT="$WORK/out42"
BASE="$OUT/$(basename "$SRC")"
ELF="$SRC/app.elf"
NUM_VERSIONS=10
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "$ELF"
printf '\x02\x00\x3e\x00\x01\x00\x00\x00\x00\x10\x40\x00\x00\x00\x00\x00' >> "$ELF"
GEN_TMP="$WORK/.gen42"
printf '\x48\x89\xe5\x48\x83\xec\x20\x48\x8b\x45\xf8\x48\x89\x45\xe0' > "$GEN_TMP"
printf '\x48\x8b\x45\xe0\x48\x8b\x00\x48\x89\x45\xd8\x48\x8b\x45\xd8' >> "$GEN_TMP"
printf '\x48\x83\xc4\x20\x5d\xc3\x90\x90\x90\x90\x90\x90\x90\x90' >> "$GEN_TMP"
printf '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >> "$GEN_TMP"
CUR=48
while [ "$CUR" -lt 1048560 ]; do
cat "$GEN_TMP" "$GEN_TMP" > "$GEN_TMP.d" 2>/dev/null && mv "$GEN_TMP.d" "$GEN_TMP"
CUR=$((CUR * 2))
done
head -c 1048560 "$GEN_TMP" >> "$ELF"
rm -f "$GEN_TMP"
ELF_MAGIC=$(head -c 4 "$ELF" | xxd -p)
assert_eq "ELF magic correct" "7f454c46" "$ELF_MAGIC"
assert_ok "create v1 (ELF base)" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
for v in $(seq 2 $NUM_VERSIONS); do
OFF1=$(( (v * 4096 + 1024) % 1048000 ))
printf 'VER_%03d_PATCH_SIG_' "$v" | dd of="$ELF" bs=1 seek="$OFF1" conv=notrunc status=none 2>/dev/null
OFF2=$(( (v * 8192 + 2048) % 1047000 ))
head -c 128 /dev/urandom | dd of="$ELF" bs=1 seek="$OFF2" conv=notrunc status=none 2>/dev/null
touch "$ELF"
sleep 1.1
assert_ok "create v$v (ELF delta)" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
done
SNAPS=$(get_snap_count "$REPO")
assert_eq "10 ELF snapshots created" "$NUM_VERSIONS" "$SNAPS"
LAST_SNAP=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore v10 (ELF)" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "ELF v10 byte-identical" cmp -s "$ELF" "$BASE/app.elf"
RESTORED_MAGIC=$(head -c 4 "$BASE/app.elf" | xxd -p)
assert_eq "ELF magic preserved after restore" "7f454c46" "$RESTORED_MAGIC"
assert_ok "verify after 10 ELF versions" "$BARESNAP" verify "$REPO"
PACKS_SIZE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
log "  [INFO] ELF 10 versions: packs = ${PACKS_SIZE} KB (without delta ~10240 KB)"
if [ "$PACKS_SIZE" -lt 5000 ]; then
SAVINGS=$((100 - PACKS_SIZE * 100 / 10240))
pass "ELF binary delta saves ~${SAVINGS}% in packs"
else
fail "ELF binary delta: packs too large (${PACKS_SIZE} KB)"
fi
fi

