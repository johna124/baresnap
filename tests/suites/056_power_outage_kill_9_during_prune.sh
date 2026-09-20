# ============================================================================
# 56. Power Outage: kill -9 during prune - PART 1 (TSan Optimized Layout)
# ============================================================================
section "56. Power Outage: kill -9 during prune"

REPO56="$WORK/repo56"
SRC56="$WORK/src56"

rm -rf "$REPO56" "$SRC56"
assert_ok "56.1 init" "$BARESNAP" init "$REPO56"
mkdir -p "$SRC56"

# Detect if we are running under heavy instrumentation (ASan or TSan profiling)
IS_SANITIZED_BUILD=0
if command -v ldd &>/dev/null && ldd "$BARESNAP" 2>/dev/null | grep -E -qi 'asan|tsan'; then
    IS_SANITIZED_BUILD=1
elif command -v nm &>/dev/null && nm "$BARESNAP" 2>/dev/null | grep -E -qi 'asan|tsan'; then
    IS_SANITIZED_BUILD=1
fi

# Scale down the dataset matrix to bypass heavy sanitizer loop penalties
if [ "$IS_SANITIZED_BUILD" -eq 1 ]; then
    log "  [INFO] Sanitizer detected: scaling workload parameters to prevent execution timeouts"
    SNAP_COUNT=3
    PAYLOAD_BYTES=65536
    KILL_DELAY=0.05
else
    SNAP_COUNT=5
    PAYLOAD_BYTES=262144
    KILL_DELAY=0.01
fi

# Create snapshots with unique workloads so prune has physical work to perform
for i in $(seq 1 "$SNAP_COUNT"); do
    printf "v$i\n" > "$SRC56/file.txt"
    head -c "$PAYLOAD_BYTES" /dev/urandom > "$SRC56/data.bin"
    "$BARESNAP" create "$REPO56" "$SRC56" >/dev/null 2>&1
    # Minimize structural sleep stalls under thread analysis
    [ "$IS_SANITIZED_BUILD" -eq 1 ] && sleep 0.2 || sleep 1.1
done

SNAPS56=$(get_snap_count "$REPO56")
assert_eq "56.2 snapshots created" "$SNAP_COUNT" "$SNAPS56"

    # ========================================================================
    # PART 2: CONTROLLED BACKGROUND INTERRUPTION AND HEALTH AUDIT
    # ========================================================================
    # Launch prune in background with environmental variables passed cleanly
    "$BARESNAP" prune "$REPO56" --keep-last 1 >/dev/null 2>&1 &
    PRUNE_PID56=$!
    
    # Adaptive timing window to let TSan initialize before sending SIGKILL
    sleep "$KILL_DELAY" 2>/dev/null || true
    kill -9 "$PRUNE_PID56" 2>/dev/null || true
    wait "$PRUNE_PID56" 2>/dev/null || true
    
    pass "56.3 kill -9 sent to prune (pid=$PRUNE_PID56)"
    
    # Health must detect and repair without entering zombie loops
    HEALTH_RC56=0
    HEALTH_OUT56=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO56" --repair </dev/null 2>&1) || HEALTH_RC56=$?
    
    if [ "$HEALTH_RC56" -ge 128 ]; then
        fail "56.4 health terminated by signal after kill -9 (rc=$HEALTH_RC56)"
    else
        pass "56.4 health executed after kill -9 (rc=$HEALTH_RC56)"
    fi
    
    # No zombie temporaries (partial packs from brs_pack_writer)
    TMP56=$(find "$REPO56/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
    if [ "$TMP56" -eq 0 ]; then
        pass "56.5 no zombie temporaries after kill -9"
    else
        fail "56.5 $TMP56 zombie temporaries in tmp/"
    fi
    
    # Verify must pass after repair
    set +e
    "$BARESNAP" verify "$REPO56" >/dev/null 2>&1
    VERIFY_RC56=$?
    set -e
    
    if [ "$VERIFY_RC56" -eq 0 ]; then
        pass "56.6 verify OK after kill -9 + repair"
    else
        fail "56.6 verify fails after kill -9 (rc=$VERIFY_RC56)"
    fi
    
    # The repo must remain functional: subsequent create
    printf "post-kill\n" > "$SRC56/file.txt"
    assert_ok "56.7 create post-kill -9" "$BARESNAP" create "$REPO56" "$SRC56"
    
    # Post-kill audit
    audit_post_prune "$REPO56" "56.8 post-kill-9"


