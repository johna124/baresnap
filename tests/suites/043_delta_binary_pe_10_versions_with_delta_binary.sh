# ============================================================
# 43. Delta binary: PE 10 versions with --delta-binary
# ============================================================
section "43. Delta binary: PE 10 versions with --delta-binary"
if ! "$BARESNAP" create --help 2>&1 | grep -q "delta-binary"; then
log "  [SKIP] --delta-binary not implemented"
else
REPO="$WORK/repo43"
SRC="$WORK/src43"
OUT="$WORK/out43"
BASE="$OUT/$(basename "$SRC")"
PE="$SRC/app.exe"
NUM_VERSIONS=10
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'MZ\x90\x00\x03\x00\x00\x00\x04\x00\x00\x00\xff\xff\x00\x00' > "$PE"
printf '\xb8\x00\x00\x00\x00\x00\x00\x00\x40\x00\x00\x00\x00\x00\x00\x00' >> "$PE"
GEN_TMP="$WORK/.gen43"
printf '\x55\x8b\xec\x83\xec\x20\x8b\x45\xfc\x89\x45\xe0\x8b\x45\xe0' > "$GEN_TMP"
printf '\x8b\x00\x89\x45\xd8\x8b\x45\xd8\x83\xc4\x20\x5d\xc3\x90\x90' >> "$GEN_TMP"
printf '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >> "$GEN_TMP"
CUR=46
while [ "$CUR" -lt 1048560 ]; do
cat "$GEN_TMP" "$GEN_TMP" > "$GEN_TMP.d" 2>/dev/null && mv "$GEN_TMP.d" "$GEN_TMP"
CUR=$((CUR * 2))
done
head -c 1048560 "$GEN_TMP" >> "$PE"
rm -f "$GEN_TMP"
PE_MAGIC=$(head -c 2 "$PE" | xxd -p)
assert_eq "PE magic correct (MZ)" "4d5a" "$PE_MAGIC"
assert_ok "create v1 (PE base)" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
for v in $(seq 2 $NUM_VERSIONS); do
OFF1=$(( (v * 4096 + 1024) % 1048000 ))
printf 'VER_%03d_PATCH_SIG_' "$v" | dd of="$PE" bs=1 seek="$OFF1" conv=notrunc status=none 2>/dev/null
OFF2=$(( (v * 8192 + 2048) % 1047000 ))
head -c 128 /dev/urandom | dd of="$PE" bs=1 seek="$OFF2" conv=notrunc status=none 2>/dev/null
touch "$PE"
sleep 1.1
assert_ok "create v$v (PE delta)" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
done
SNAPS=$(get_snap_count "$REPO")
assert_eq "10 PE snapshots created" "$NUM_VERSIONS" "$SNAPS"
LAST_SNAP=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore v10 (PE)" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "PE v10 byte-identical" cmp -s "$PE" "$BASE/app.exe"
RESTORED_MAGIC=$(head -c 2 "$BASE/app.exe" | xxd -p)
assert_eq "PE magic preserved after restore" "4d5a" "$RESTORED_MAGIC"
assert_ok "verify after 10 PE versions" "$BARESNAP" verify "$REPO"
PACKS_SIZE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
log "  [INFO] PE 10 versions: packs = ${PACKS_SIZE} KB (without delta ~10240 KB)"
if [ "$PACKS_SIZE" -lt 5000 ]; then
SAVINGS=$((100 - PACKS_SIZE * 100 / 10240))
pass "PE binary delta saves ~${SAVINGS}% in packs"
else
fail "PE binary delta: packs too large (${PACKS_SIZE} KB)"
fi
fi

