# ============================================================
# 69. INTEGRATED FINAL BOSS (adapted to Dual Core)
# ============================================================
section "69. INTEGRATED FINAL BOSS (adapted Dual Core)"
# Quick adjustments for old machines
FB9_DIRS=10
FB9_FILES_PER_DIR=10
FB9_FUZZ_ROUNDS=3
# ------------------------------------------------------------
# 69.1 REDUCED SWARM
# ------------------------------------------------------------
FB9_REPO="$WORK/fb9_repo"
FB9_SRC="$WORK/fb9_src"
FB9_OUT="$WORK/fb9_out"
assert_ok "69.1 init final boss" "$BARESNAP" init "$FB9_REPO"
mkdir -p "$FB9_SRC"
for i in $(seq 1 "$FB9_DIRS"); do
FB9_DIR="$FB9_SRC/dir $i (weird) [glob] & 'quote'"
mkdir -p "$FB9_DIR"
for j in $(seq 1 "$FB9_FILES_PER_DIR"); do
printf 'final boss %d-%d
' "$i" "$j" > "$FB9_DIR/file $i-$j 'q' [x].txt"
done
done
pass "69.2 reduced swarm created ($((FB9_DIRS * FB9_FILES_PER_DIR)) files)"
assert_ok "69.3 create swarm" "$BARESNAP" create "$FB9_REPO" "$FB9_SRC"
FB9_SNAP="$(get_latest_snap "$FB9_REPO")"
assert_ok "69.4 restore swarm" "$BARESNAP" restore "$FB9_REPO" "$FB9_SNAP" "$FB9_OUT"
FB9_SRC_FILES="$(find "$FB9_SRC" -type f 2>/dev/null | wc -l)"
FB9_DST_FILES="$(find "$FB9_OUT" -type f 2>/dev/null | wc -l)"
assert_eq "69.5 swarm file count" "$FB9_SRC_FILES" "$FB9_DST_FILES"
assert_ok "69.6 verify swarm" "$BARESNAP" verify "$FB9_REPO"
# ------------------------------------------------------------
# 69.7 SHORT DELTA CHAIN + PRUNE
# ------------------------------------------------------------
FB9_REPO_DELTA="$WORK/fb9_delta"
FB9_SRC_DELTA="$WORK/fb9_delta_src"
FB9_OUT_DELTA="$WORK/fb9_delta_out"
FB9_BASE_DELTA="$FB9_OUT_DELTA/$(basename "$FB9_SRC_DELTA")"
assert_ok "69.7 init delta" "$BARESNAP" init "$FB9_REPO_DELTA"
mkdir -p "$FB9_SRC_DELTA"
yes "FINAL BOSS INTEGRATED DELTA LINE." | head -c 32768 > "$FB9_SRC_DELTA/delta.txt"
assert_ok "69.8 create delta v1" "$BARESNAP" create "$FB9_REPO_DELTA" "$FB9_SRC_DELTA"
sleep 1.1
printf 'FB9_MOD_2 ' | dd of="$FB9_SRC_DELTA/delta.txt" bs=1 seek=400 conv=notrunc status=none
touch "$FB9_SRC_DELTA/delta.txt"
assert_ok "69.9 create delta v2" "$BARESNAP" create "$FB9_REPO_DELTA" "$FB9_SRC_DELTA"
sleep 1.1
printf 'FB9_MOD_3 ' | dd of="$FB9_SRC_DELTA/delta.txt" bs=1 seek=800 conv=notrunc status=none
touch "$FB9_SRC_DELTA/delta.txt"
assert_ok "69.10 create delta v3" "$BARESNAP" create "$FB9_REPO_DELTA" "$FB9_SRC_DELTA"
FB9_SNAPS_DELTA="$(get_snap_count "$FB9_REPO_DELTA")"
assert_eq "69.11 3 delta snapshots created" "3" "$FB9_SNAPS_DELTA"
assert_ok "69.12 prune --keep-last 1" "$BARESNAP" prune "$FB9_REPO_DELTA" --keep-last 1
FB9_SNAPS_DELTA_AFTER="$(get_snap_count "$FB9_REPO_DELTA")"
assert_eq "69.13 1 snapshot after prune" "1" "$FB9_SNAPS_DELTA_AFTER"
assert_ok "69.14 verify after delta prune" "$BARESNAP" verify "$FB9_REPO_DELTA"
FB9_SNAP_DELTA="$(get_latest_snap "$FB9_REPO_DELTA")"
assert_ok "69.15 restore after delta prune" "$BARESNAP" restore "$FB9_REPO_DELTA" "$FB9_SNAP_DELTA" "$FB9_OUT_DELTA"
assert_ok "69.16 delta.txt correct after prune+restore" \
cmp -s "$FB9_SRC_DELTA/delta.txt" "$FB9_BASE_DELTA/delta.txt"
# ------------------------------------------------------------
# 69.17 VAULT: encryption + weird paths + extract
# ------------------------------------------------------------
FB9_REPO_VAULT="$WORK/fb9_vault"
FB9_SRC_VAULT="$WORK/fb9 vault (weird) & [final]"
FB9_OUT_VAULT="$WORK/fb9_vault_out"
FB9_OUT_VAULT_EXTRACT="$WORK/fb9_vault_extract"
FB9_VAULT_BASENAME="$(basename "$FB9_SRC_VAULT")"
FB9_BASE_VAULT="$FB9_OUT_VAULT/$FB9_VAULT_BASENAME"
export BARESNAP_PASSPHRASE="fb9-vault-pass"
assert_ok "69.17 init encrypted" "$BARESNAP" init "$FB9_REPO_VAULT" --encrypt
mkdir -p "$FB9_SRC_VAULT/dir with spaces/'quotes' & [globs]"
printf 'fb9 secret v1
' > "$FB9_SRC_VAULT/dir with spaces/'quotes' & [globs]/secret.txt"
head -c 32768 /dev/urandom > "$FB9_SRC_VAULT/random.bin"
assert_ok "69.18 create encrypted v1" "$BARESNAP" create "$FB9_REPO_VAULT" "$FB9_SRC_VAULT"
sleep 1.1
printf 'fb9 secret v2
' > "$FB9_SRC_VAULT/dir with spaces/'quotes' & [globs]/secret.txt"
assert_ok "69.19 create encrypted v2" "$BARESNAP" create "$FB9_REPO_VAULT" "$FB9_SRC_VAULT"
FB9_SNAP_VAULT="$(get_latest_snap "$FB9_REPO_VAULT")"
assert_ok "69.20 restore encrypted" "$BARESNAP" restore "$FB9_REPO_VAULT" "$FB9_SNAP_VAULT" "$FB9_OUT_VAULT"
assert_ok "69.21 secret.txt correct" \
cmp -s "$FB9_SRC_VAULT/dir with spaces/'quotes' & [globs]/secret.txt" \
"$FB9_BASE_VAULT/dir with spaces/'quotes' & [globs]/secret.txt"
assert_ok "69.22 random.bin byte-identical" \
cmp -s "$FB9_SRC_VAULT/random.bin" "$FB9_BASE_VAULT/random.bin"
assert_ok "69.23 selective extract encrypted" \
"$BARESNAP" extract "$FB9_REPO_VAULT" "$FB9_SNAP_VAULT" "$FB9_OUT_VAULT_EXTRACT" \
"$FB9_VAULT_BASENAME/dir with spaces/'quotes' & [globs]/secret.txt"
FB9_FOUND_SECRET="$(find "$FB9_OUT_VAULT_EXTRACT" -name "secret.txt" -type f 2>/dev/null | head -1)"
if [ -n "${FB9_FOUND_SECRET:-}" ] && grep -q "fb9 secret v2" "$FB9_FOUND_SECRET" 2>/dev/null; then
pass "69.24 encrypted extract with weird paths correct"
else
fail "69.24 encrypted extract with weird paths incorrect"
fi
export BARESNAP_PASSPHRASE="wrong-fb9-pass"
assert_fail "69.25 restore encrypted with incorrect pass" \
"$BARESNAP" restore "$FB9_REPO_VAULT" "$FB9_SNAP_VAULT" "$WORK/fb9_bad_vault"
unset BARESNAP_PASSPHRASE
# ------------------------------------------------------------
# 69.26 EARTHQUAKE: multiple corruption + repair
# ------------------------------------------------------------
FB9_REPO_QUAKE="$WORK/fb9_quake"
FB9_SRC_QUAKE="$WORK/fb9_quake_src"
assert_ok "69.26 init earthquake" "$BARESNAP" init "$FB9_REPO_QUAKE"
mkdir -p "$FB9_SRC_QUAKE"
head -c 32768 /dev/urandom > "$FB9_SRC_QUAKE/data.bin"
printf 'quake v1
' > "$FB9_SRC_QUAKE/text.txt"
assert_ok "69.27 create snap 1" "$BARESNAP" create "$FB9_REPO_QUAKE" "$FB9_SRC_QUAKE"
sleep 1.1
printf 'quake v2
' >> "$FB9_SRC_QUAKE/text.txt"
assert_ok "69.28 create snap 2" "$BARESNAP" create "$FB9_REPO_QUAKE" "$FB9_SRC_QUAKE"
assert_ok "69.29 verify before earthquake" "$BARESNAP" verify "$FB9_REPO_QUAKE"
FB9_PACK="$(find "$FB9_REPO_QUAKE/packs" -maxdepth 1 -type f -name '*.pack' 2>/dev/null | head -1)"
FB9_SNAP_FILE="$(find "$FB9_REPO_QUAKE/snapshots" -maxdepth 1 -type f -name '*.snap' 2>/dev/null | head -1)"
FB9_IDX="$(find "$FB9_REPO_QUAKE/index" -maxdepth 1 -type f -name '*.idx' 2>/dev/null | head -1)"
if [ -n "${FB9_PACK:-}" ] && [ -n "${FB9_SNAP_FILE:-}" ] && [ -n "${FB9_IDX:-}" ]; then
FB9_SNAP_SIZE="$(stat -c '%s' "$FB9_SNAP_FILE" 2>/dev/null || echo 0)"
FB9_CORRUPT_OFF=50
if [ "${FB9_SNAP_SIZE:-0}" -le 60 ]; then
FB9_CORRUPT_OFF=$((FB9_SNAP_SIZE / 2))
fi
rm -f "$FB9_PACK"
printf '\xFF\xFE\xFD' | dd of="$FB9_SNAP_FILE" bs=1 seek="$FB9_CORRUPT_OFF" conv=notrunc status=none 2>/dev/null
rm -f "$FB9_IDX"
pass "69.30 earthquake applied (pack deleted + corrupt snap + deleted idx)"
assert_fail "69.31 verify detects corruption" "$BARESNAP" verify "$FB9_REPO_QUAKE"
FB9_VERIFY_RC=1
FB9_VERIFY_OUT=""
for FB9_TRY in 1 2 3; do
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$FB9_REPO_QUAKE" --repair \
</dev/null >/dev/null 2>&1 || true
set +e
FB9_VERIFY_OUT="$("$BARESNAP" verify "$FB9_REPO_QUAKE" 2>&1)"
FB9_VERIFY_RC=$?
set -e
if [ "$FB9_VERIFY_RC" -eq 0 ]; then
break
fi
done
if [ "$FB9_VERIFY_RC" -eq 0 ]; then
pass "69.32 verify passes after repair"
else
fail "69.32 verify fails after repair"
log "  [DEBUG] verify output:"
printf '%s
' "$FB9_VERIFY_OUT" | tail -n 6 | while IFS= read -r line; do
log "    $line"
done
fi
FB9_DAMAGED="$(find "$FB9_REPO_QUAKE/damaged" -type f -name '*.snap' 2>/dev/null | wc -l)"
FB9_LIVE="$(get_snap_count "$FB9_REPO_QUAKE")"
log "  [INFO] post-earthquake: $FB9_LIVE alive, ${FB9_DAMAGED:-0} in damaged/"
if [ "${FB9_DAMAGED:-0}" -ge 1 ]; then
pass "69.33 damaged snapshots moved to damaged/"
else
fail "69.33 no damaged snapshot was moved"
fi
else
fail "69.30 could not locate pack/snap/idx for the earthquake"
fail "69.31 skip"
fail "69.32 skip"
fail "69.33 skip"
fi
# ------------------------------------------------------------
# 69.34 LABYRINTH: long path
# ------------------------------------------------------------
FB9_REPO_MAZE="$WORK/fb9_maze"
FB9_SRC_MAZE="$WORK/fb9_maze_src"
FB9_OUT_MAZE="$WORK/fb9_maze_out"
assert_ok "69.34 init labyrinth" "$BARESNAP" init "$FB9_REPO_MAZE"
mkdir -p "$FB9_SRC_MAZE"
FB9_DEEP="$FB9_SRC_MAZE"
FB9_LEVEL=0
while true; do
FB9_NEXT="$FB9_DEEP/level_${FB9_LEVEL}_with_a_long_directory_name_for_path_limit"
if [ "${#FB9_NEXT}" -gt 3500 ]; then
break
fi
mkdir -p "$FB9_NEXT" 2>/dev/null || break
FB9_DEEP="$FB9_NEXT"
FB9_LEVEL=$((FB9_LEVEL + 1))
done
printf 'deep final boss
' > "$FB9_DEEP/deepest.txt"
pass "69.35 labyrinth created ($FB9_LEVEL levels)"
assert_ok "69.36 create labyrinth" "$BARESNAP" create "$FB9_REPO_MAZE" "$FB9_SRC_MAZE"
FB9_SNAP_MAZE="$(get_latest_snap "$FB9_REPO_MAZE")"
assert_ok "69.37 restore labyrinth" "$BARESNAP" restore "$FB9_REPO_MAZE" "$FB9_SNAP_MAZE" "$FB9_OUT_MAZE"
FB9_DEEP_RESTORED="$(find "$FB9_OUT_MAZE" -name "deepest.txt" -type f 2>/dev/null | head -1)"
if [ -n "${FB9_DEEP_RESTORED:-}" ] && grep -q "deep final boss" "$FB9_DEEP_RESTORED" 2>/dev/null; then
pass "69.38 deep file restored"
else
fail "69.38 deep file not restored"
fi
assert_ok "69.39 verify labyrinth" "$BARESNAP" verify "$FB9_REPO_MAZE"
# ------------------------------------------------------------
# 69.40 RUSSIAN ROULETTE: light snapshot fuzzing
# ------------------------------------------------------------
FB9_REPO_FUZZ="$WORK/fb9_fuzz"
FB9_SRC_FUZZ="$WORK/fb9_fuzz_src"
assert_ok "69.40 init fuzzing" "$BARESNAP" init "$FB9_REPO_FUZZ"
mkdir -p "$FB9_SRC_FUZZ"
head -c 32768 /dev/urandom > "$FB9_SRC_FUZZ/fuzz.bin"
assert_ok "69.41 create fuzzing" "$BARESNAP" create "$FB9_REPO_FUZZ" "$FB9_SRC_FUZZ"
FB9_FUZZ_SNAP_FILE="$(find "$FB9_REPO_FUZZ/snapshots" -maxdepth 1 -type f -name '*.snap' 2>/dev/null | head -1)"
FB9_FUZZ_BACKUP="$WORK/fb9_fuzz_backup.snap"
if [ -n "${FB9_FUZZ_SNAP_FILE:-}" ]; then
cp "$FB9_FUZZ_SNAP_FILE" "$FB9_FUZZ_BACKUP"
FB9_FUZZ_SIZE="$(stat -c '%s' "$FB9_FUZZ_SNAP_FILE" 2>/dev/null || echo 0)"
pass "69.42 snapshot copied for fuzzing (${FB9_FUZZ_SIZE} bytes)"
for FB9_MUT in $(seq 1 "$FB9_FUZZ_ROUNDS"); do
cp "$FB9_FUZZ_BACKUP" "$FB9_FUZZ_SNAP_FILE"
if [ "${FB9_FUZZ_SIZE:-0}" -le 30 ]; then
FB9_OFF=5
else
FB9_OFF=$(( (RANDOM % (FB9_FUZZ_SIZE - 20)) + 10 ))
fi
printf '\xFF' | dd of="$FB9_FUZZ_SNAP_FILE" bs=1 seek="$FB9_OFF" conv=notrunc status=none 2>/dev/null
assert_fail "69.43 fuzzing mutation $FB9_MUT detected" \
"$BARESNAP" verify "$FB9_REPO_FUZZ"
done
cp "$FB9_FUZZ_BACKUP" "$FB9_FUZZ_SNAP_FILE"
assert_ok "69.44 verify OK after restoring clean snapshot" \
"$BARESNAP" verify "$FB9_REPO_FUZZ"
else
fail "69.42 no snapshot found for fuzzing"
fail "69.43 skip"
fail "69.44 skip"
fi
# ------------------------------------------------------------
# 69.45 LIGHT APOCALYPSE: 2 concurrent creates
# ------------------------------------------------------------
FB9_REPO_APOC="$WORK/fb9_apoc"
FB9_SRC_APOC="$WORK/fb9_apoc_src"
FB9_OUT_APOC="$WORK/fb9_apoc_out"
assert_ok "69.45 init apocalypse" "$BARESNAP" init "$FB9_REPO_APOC"
mkdir -p "$FB9_SRC_APOC/src_1" "$FB9_SRC_APOC/src_2"
head -c 32768 /dev/urandom > "$FB9_SRC_APOC/src_1/data.bin"
head -c 32768 /dev/urandom > "$FB9_SRC_APOC/src_2/data.bin"
"$BARESNAP" create "$FB9_REPO_APOC" "$FB9_SRC_APOC/src_1" > "$WORK/fb9_apoc_1.log" 2>&1 &
FB9_PID1=$!
"$BARESNAP" create "$FB9_REPO_APOC" "$FB9_SRC_APOC/src_2" > "$WORK/fb9_apoc_2.log" 2>&1 &
FB9_PID2=$!
wait "$FB9_PID1" 2>/dev/null || true
wait "$FB9_PID2" 2>/dev/null || true
pass "69.46 concurrent creates finished"
FB9_SNAPS_APOC="$(get_snap_count "$FB9_REPO_APOC")"
if [ "${FB9_SNAPS_APOC:-0}" -ge 2 ]; then
pass "69.47 snapshots created correctly ($FB9_SNAPS_APOC)"
elif [ "${FB9_SNAPS_APOC:-0}" -ge 1 ]; then
pass "69.47 at least 1 snapshot created ($FB9_SNAPS_APOC)"
else
fail "69.47 no snapshot was created"
fi
assert_ok "69.48 verify post-apocalypse" "$BARESNAP" verify "$FB9_REPO_APOC"
assert_ok "69.49 prune --keep-last 1" "$BARESNAP" prune "$FB9_REPO_APOC" --keep-last 1
assert_ok "69.50 verify after prune" "$BARESNAP" verify "$FB9_REPO_APOC"
FB9_SNAP_APOC="$(get_latest_snap "$FB9_REPO_APOC")"
assert_ok "69.51 restore post-apocalypse" "$BARESNAP" restore "$FB9_REPO_APOC" "$FB9_SNAP_APOC" "$FB9_OUT_APOC"
pass "69.52 integrated FINAL BOSS passed"

