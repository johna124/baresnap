# ============================================================
# 92. SSH: Remote LIST with long names
# ============================================================
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
section "92. SSH: Remote LIST with long names"
SSH_TARGET92="$(whoami)@localhost"
REMOTE_PATH92="/tmp/baresnap_smoke_hl_test"
SSH_URI92="ssh://${SSH_TARGET92}${REMOTE_PATH92}"
SSH_SRC92="$WORK/ssh_src_hl"
ssh "$SSH_TARGET92" "rm -rf $REMOTE_PATH92" 2>/dev/null || true
mkdir -p "$SSH_SRC92/subdir"
LONG_NAME92=$(printf 'B%.0s' $(seq 1 200))
LONG_NAME92="${LONG_NAME92}.txt"
printf 'ssh long name
' > "$SSH_SRC92/$LONG_NAME92"
printf 'normal
' > "$SSH_SRC92/normal.txt"
printf 'nested
' > "$SSH_SRC92/subdir/nested.txt"
assert_ok "92.1 remote install" "$BARESNAP" remote install "$SSH_URI92"
assert_ok "92.2 remote init" "$BARESNAP" init "$SSH_URI92"
assert_ok "92.3 remote create with long name" "$BARESNAP" create "$SSH_URI92" "$SSH_SRC92"
SSH_SNAP92=$("$BARESNAP" list "$SSH_URI92" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
LS_OUT92=$("$BARESNAP" ls "$SSH_URI92" "$SSH_SNAP92" --recursive 2>/dev/null)
LS_RC92=$?
assert_eq "92.4 remote ls works" "0" "$LS_RC92"
if printf '%s
' "$LS_OUT92" | grep -q "BBBB"; then
pass "92.5 remote ls shows long file"
else
fail "92.5 remote ls does not show long file"
fi
SSH_OUT92="$WORK/ssh_out_hl"
assert_ok "92.6 remote restore with long name" "$BARESNAP" restore "$SSH_URI92" "$SSH_SNAP92" "$SSH_OUT92"
SSH_BASE92="$SSH_OUT92/$(basename "$SSH_SRC92")"
if [ -f "$SSH_BASE92/$LONG_NAME92" ]; then
pass "92.7 long file restored correctly via SSH"
else
fail "92.7 long file NOT restored via SSH"
fi
assert_ok "92.8 remote verify" "$BARESNAP" verify "$SSH_URI92"
ssh "$SSH_TARGET92" "rm -rf $REMOTE_PATH92" 2>/dev/null || true
else
section "92. SSH: Remote LIST with long names"
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi

