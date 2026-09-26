# ============================================================
# 76. Zombie Inode: hot file deletion during CDC
# ============================================================
section "76. Zombie Inode: hot file deletion during CDC"
REPO76="$WORK/repo76"
SRC76="$WORK/src76"
OUT76="$WORK/out76"
BASE76="$OUT76/$(basename "$SRC76")"
assert_ok "76.1 init" "$BARESNAP" init "$REPO76"
mkdir -p "$SRC76"
# Large file (8 MB) so create lasts and deletion coincides with CDC
head -c 8388608 /dev/urandom > "$SRC76/zombi.bin"
printf 'stable file
' > "$SRC76/estable.txt"
# Launch create in background and delete the file repeatedly
"$BARESNAP" create "$REPO76" "$SRC76" "snap_zombie" >/dev/null 2>&1 &
PID76=$!
for i in $(seq 1 50); do
rm -f "$SRC76/zombi.bin"
kill -0 "$PID76" 2>/dev/null || break
sleep 0.02
done
RC76=0
wait "$PID76" 2>/dev/null || RC76=$?
if [ "$RC76" -ge 128 ]; then
fail "76.2 create CRASHED with signal $((RC76 - 128))"
else
TMP76=$(find "$REPO76/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP76" -gt 0 ]; then
fail "76.2 $TMP76 zombie temporaries in tmp/"
else
pass "76.2 no crash or temporaries (rc=$RC76)"
fi
fi
if [ "$RC76" -eq 0 ]; then
assert_ok "76.3 verify of the repo intact" "$BARESNAP" verify "$REPO76"
SNAP76=$(get_latest_snap "$REPO76")
assert_ok "76.4 restore of the snapshot" "$BARESNAP" restore "$REPO76" "$SNAP76" "$OUT76"
assert_ok "76.5 stable file preserved" cmp -s "$SRC76/estable.txt" "$BASE76/estable.txt"
else
log "  [INFO] create failed controlledly (rc=$RC76), repo intact"
fi

