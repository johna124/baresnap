# ============================================================
# 103. Integrated regression post-fixes (full cycle)
# ============================================================
section "103. Integrated regression post-fixes (full cycle)"
REPO_REG="$WORK/repo_reg"
SRC_REG="$WORK/src_reg"
OUT_REG="$WORK/out_reg"
mkdir -p "$SRC_REG/sub"
printf 'regression test v1
' > "$SRC_REG/file.txt"
printf 'nested
' > "$SRC_REG/sub/nested.txt"
head -c 32768 /dev/urandom > "$SRC_REG/blob.bin"
assert_ok "103.1 init" "$BARESNAP" init "$REPO_REG"
assert_ok "103.2 create v1" "$BARESNAP" create "$REPO_REG" "$SRC_REG"
sleep 1.1
printf 'regression test v2
' > "$SRC_REG/file.txt"
assert_ok "103.3 create v2" "$BARESNAP" create "$REPO_REG" "$SRC_REG"
assert_ok "103.4 list" "$BARESNAP" list "$REPO_REG"
assert_ok "103.5 info" "$BARESNAP" info "$REPO_REG"
assert_ok "103.6 verify" "$BARESNAP" verify "$REPO_REG"
assert_ok "103.7 prune --keep-last 1" "$BARESNAP" prune "$REPO_REG" --keep-last 1
assert_ok "103.8 verify after prune" "$BARESNAP" verify "$REPO_REG"
SNAP_REG=$("$BARESNAP" list "$REPO_REG" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
assert_ok "103.9 restore" "$BARESNAP" restore "$REPO_REG" "$SNAP_REG" "$OUT_REG"
BASE_REG="$OUT_REG/$(basename "$SRC_REG")"
assert_ok "103.10 file.txt correct" cmp -s "$SRC_REG/file.txt" "$BASE_REG/file.txt"
assert_ok "103.11 nested.txt correct" cmp -s "$SRC_REG/sub/nested.txt" "$BASE_REG/sub/nested.txt"
assert_ok "103.12 blob.bin correct" cmp -s "$SRC_REG/blob.bin" "$BASE_REG/blob.bin"
assert_ok "103.13 diff" "$BARESNAP" diff "$REPO_REG" "$SNAP_REG" "$SNAP_REG"

