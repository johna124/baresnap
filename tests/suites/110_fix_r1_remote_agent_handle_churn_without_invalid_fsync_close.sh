# ============================================================
# 110. FIX R1: Remote agent — handle churn without invalid fsync/close
# ============================================================
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
section "110. FIX R1: remote agent — handle churn"
SSH_TARGET110="$(whoami)@localhost"
REMOTE_PATH110="/tmp/baresnap_mega_r1_handles"
SSH_URI110="ssh://${SSH_TARGET110}${REMOTE_PATH110}"
SSH_SRC110="$WORK/ssh_src_r1"
SSH_OUT110="$WORK/ssh_out_r1"
ssh "$SSH_TARGET110" "rm -rf $REMOTE_PATH110" 2>/dev/null || true
mkdir -p "$SSH_SRC110/sub"
printf 'handle churn test
' > "$SSH_SRC110/a.txt"
for i in $(seq 1 20); do
head -c 8192 /dev/urandom > "$SSH_SRC110/sub/f_$i.bin"
done
assert_ok "110.1 remote install" "$BARESNAP" remote install "$SSH_URI110"
assert_ok "110.2 remote test" "$BARESNAP" remote test "$SSH_URI110"
assert_ok "110.3 remote init" "$BARESNAP" init "$SSH_URI110"
# Cycle 1: create + restore (dozens of open/close on the agent)
assert_ok "110.4 remote create (cycle 1)" "$BARESNAP" create "$SSH_URI110" "$SSH_SRC110"
SNAP110=$(get_latest_snap "$SSH_URI110")
assert_ok "110.5 remote restore (cycle 1)" "$BARESNAP" restore "$SSH_URI110" "$SNAP110" "$SSH_OUT110"
SSH_BASE110="$SSH_OUT110/$(basename "$SSH_SRC110")"
assert_ok "110.6 correct content (cycle 1)" cmp -s "$SSH_SRC110/a.txt" "$SSH_BASE110/a.txt"
# Cycle 2 on the SAME repo: forces reuse and release of slots
printf 'churn v2
' >> "$SSH_SRC110/a.txt"
assert_ok "110.7 remote create (cycle 2)" "$BARESNAP" create "$SSH_URI110" "$SSH_SRC110"
assert_ok "110.8 remote verify" "$BARESNAP" verify "$SSH_URI110"
rm -rf "$SSH_OUT110"
SNAP110B=$(get_latest_snap "$SSH_URI110")
assert_ok "110.9 remote restore (cycle 2)" "$BARESNAP" restore "$SSH_URI110" "$SNAP110B" "$SSH_OUT110"
assert_ok "110.10 v2 content correct" cmp -s "$SSH_SRC110/a.txt" "$SSH_BASE110/a.txt"
# Metadata burst: repeated list/info => read-only open/close
# (fsync on O_RDONLY fd must be tolerated without aborting the agent)
RACE110_OK=1
for i in 1 2 3; do
"$BARESNAP" list "$SSH_URI110" >/dev/null 2>&1 || RACE110_OK=0
"$BARESNAP" info "$SSH_URI110" >/dev/null 2>&1 || RACE110_OK=0
done
assert_eq "110.11 burst list/info without errors" "1" "$RACE110_OK"
assert_ok "110.12 final remote verify" "$BARESNAP" verify "$SSH_URI110"
ssh "$SSH_TARGET110" "rm -rf $REMOTE_PATH110" 2>/dev/null || true
else
section "110. FIX R1: remote agent — handle churn"
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi

