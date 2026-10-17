# ============================================================
# 96. Bug #4: comp_buf leak in join_and_fail
# ============================================================
section "96. Bug #4: comp_buf leak in join_and_fail"
REPO_B4="$WORK/repo_b4"
SRC_B4="$WORK/src_b4"
mkdir -p "$SRC_B4"
printf 'leak test
' > "$SRC_B4/file.txt"
assert_ok "96.1 init" "$BARESNAP" init "$REPO_B4"
assert_ok "96.2 create normal" "$BARESNAP" create "$REPO_B4" "$SRC_B4"
TMP_NORMAL=$(find "$REPO_B4/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP_NORMAL" -eq 0 ]; then pass "96.3 no temporaries after normal create"; else fail "96.3 $TMP_NORMAL temporaries after normal create"; fi
set +e
"$BARESNAP" create "$REPO_B4" "/nonexistent/path/that/does/not/exist" >/dev/null 2>&1 </dev/null
BAD_RC=$?
set -e
if [ "$BAD_RC" -ne 0 ]; then pass "96.4 create with invalid path failed cleanly (rc=$BAD_RC)"; else fail "96.4 create with invalid path did not fail"; fi
TMP_BAD=$(find "$REPO_B4/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP_BAD" -eq 0 ]; then pass "96.5 no temporaries after failed create"; else fail "96.5 $TMP_BAD temporaries after failed create"; fi
assert_ok "96.6 verify after failed create" "$BARESNAP" verify "$REPO_B4"

