# ============================================================
# 44. Delta binary: savings vs no delta (real ELF)
# ============================================================
section "44. Delta binary: space savings vs no delta (real ELF)"
if ! "$BARESNAP" create --help 2>&1 | grep -q "delta-binary"; then
log "  [SKIP] --delta-binary not implemented"
else
REPO_DELTA="$WORK/repo44_delta"
REPO_NODLT="$WORK/repo44_nodelta"
SRC="$WORK/src44"
BIN="$SRC/lib.so"
pack_bytes() {
local repo="$1"
local total=0
local f sz
for f in "$repo"/packs/*.pack; do
[ -f "$f" ] || continue
sz=$(stat -c '%s' "$f" 2>/dev/null || echo 0)
total=$((total + sz))
done
echo "$total"
}
is_large_elf() {
local f="$1"
local magic sz
[ -f "$f" ] || return 1
magic=$(head -c 4 "$f" 2>/dev/null | xxd -p 2>/dev/null)
[ "$magic" = "7f454c46" ] || return 1
sz=$(stat -c '%s' "$f" 2>/dev/null || echo 0)
[ "$sz" -gt 262144 ] || return 1
return 0
}
insert_patch() {
local f="$1"
local tag="$2"
local size off tmp
size=$(stat -c '%s' "$f" 2>/dev/null || echo 0)
[ "$size" -gt 64 ] || return 1
tmp="$f.tmp.$$"
for off in $((size / 2)) 16; do
[ "$off" -lt 16 ] && off=16
[ "$off" -gt $((size - 16)) ] && off=$((size - 16))
head -c "$off" "$f" > "$tmp"
printf 'BRS_REAL_PATCH_%s_%08d_' "$tag" "$off" >> "$tmp"
tail -c +$((off + 1)) "$f" >> "$tmp"
cat "$tmp" > "$f"
done
rm -f "$tmp"
touch "$f"
}
mkdir -p "$SRC"
REAL_BIN=""
if is_large_elf "$BARESNAP"; then
REAL_BIN="$BARESNAP"
else
for c in /bin/ls /usr/bin/ls /bin/sh /usr/bin/sh /usr/bin/env /usr/bin/git; do
if is_large_elf "$c"; then
REAL_BIN="$c"
break
fi
done
fi
if [ -z "$REAL_BIN" ]; then
log "  [SKIP] no real ELF binary >256 KB available"
else
cp "$REAL_BIN" "$BIN"
log "  [INFO] using real binary: $REAL_BIN ($(stat -c '%s' "$BIN") bytes)"
assert_ok "init repo with delta" "$BARESNAP" init "$REPO_DELTA"
assert_ok "init repo without delta" "$BARESNAP" init "$REPO_NODLT"
assert_ok "create delta v1" "$BARESNAP" create "$REPO_DELTA" "$SRC" --delta-binary
assert_ok "create nodelta v1" "$BARESNAP" create "$REPO_NODLT" "$SRC"
for v in 2 3 4 5 6; do
insert_patch "$BIN" "$v"
sleep 1.1
assert_ok "create delta v$v" "$BARESNAP" create "$REPO_DELTA" "$SRC" --delta-binary
assert_ok "create nodelta v$v" "$BARESNAP" create "$REPO_NODLT" "$SRC"
done
INFO_DELTA=$("$BARESNAP" info "$REPO_DELTA" 2>&1)
if printf '%s
' "$INFO_DELTA" | grep -q "delta entries"; then
pass "info shows delta entries in repo with --delta-binary"
else
fail "info does not show delta entries in repo with --delta-binary"
fi
SIZE_DELTA=$(pack_bytes "$REPO_DELTA")
SIZE_NODLT=$(pack_bytes "$REPO_NODLT")
log "  [INFO] =================================================="
log "  [INFO] COMPARISON: real binary delta vs no delta"
log "  [INFO] =================================================="
log "  [INFO] With --delta-binary:   ${SIZE_DELTA} bytes"
log "  [INFO] Without --delta-binary:   ${SIZE_NODLT} bytes"
if [ "$SIZE_NODLT" -gt 0 ] && [ "$SIZE_DELTA" -lt "$SIZE_NODLT" ]; then
SAVINGS=$(( (SIZE_NODLT - SIZE_DELTA) * 100 / SIZE_NODLT ))
log "  [INFO] Savings:               ~${SAVINGS}%"
pass "real binary delta saves space (${SAVINGS}% less)"
elif [ "$SIZE_DELTA" -eq "$SIZE_NODLT" ]; then
log "  [INFO] Savings:               0% (no difference)"
fail "real binary delta DOES NOT save space (same size)"
else
fail "real binary delta: worse than no delta (${SIZE_DELTA} vs ${SIZE_NODLT} bytes)"
fi
log "  [INFO] =================================================="
assert_ok "verify repo delta" "$BARESNAP" verify "$REPO_DELTA"
assert_ok "verify repo nodelta" "$BARESNAP" verify "$REPO_NODLT"
fi
fi

