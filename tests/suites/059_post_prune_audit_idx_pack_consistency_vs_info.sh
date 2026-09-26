# ============================================================================
# 59. Post-Prune Audit: idx/pack consistency vs info - PART 1 (TSan Optimized)
# ============================================================================
section "59. Post-Prune Audit: idx/pack consistency vs info"

REPO59="$WORK/repo59"
SRC59="$WORK/src59"

rm -rf "$REPO59" "$SRC59"
assert_ok "59.1 init" "$BARESNAP" init "$REPO59"
mkdir -p "$SRC59"

# Detect if the binary is running heavy compiler tracking instrumentation
IS_SANITIZED_BUILD=0
if command -v ldd &>/dev/null && ldd "$BARESNAP" 2>/dev/null | grep -E -qi 'asan|tsan'; then
    IS_SANITIZED_BUILD=1
elif command -v nm &>/dev/null && nm "$BARESNAP" 2>/dev/null | grep -E -qi 'asan|tsan'; then
    IS_SANITIZED_BUILD=1
fi

# Dynamically scale down the loop constraints to bypass the heavy timing penalty
if [ "$IS_SANITIZED_BUILD" -eq 1 ]; then
    log "  [INFO] Sanitizer build detected: scaling loop bounds to eliminate timeout freezes"
    SNAP_COUNT=3
    PAYLOAD_BYTES=16384
else
    SNAP_COUNT=5
    PAYLOAD_BYTES=32768
fi

# Build snapshots with real variations so prune has delta verification workloads
for i in $(seq 1 "$SNAP_COUNT"); do
    printf "audit v$i\n" > "$SRC59/file.txt"
    head -c "$PAYLOAD_BYTES" /dev/urandom > "$SRC59/data.bin"
    "$BARESNAP" create "$REPO59" "$SRC59" >/dev/null 2>&1
    
    # Minimize artificial timing stalls under microthread analysis
    [ "$IS_SANITIZED_BUILD" -eq 1 ] && sleep 0.1 || sleep 1.1
done

SNAPS59=$(get_snap_count "$REPO59")
assert_eq "59.2 snapshots before prune" "$SNAP_COUNT" "$SNAPS59"

    # ========================================================================
    # PART 2: AGGRESSIVE PURGE VERIFICATION & GEOMETRIC DISK AUDIT
    # ========================================================================
    # Aggressive prune: keep-last 1
    assert_ok "59.3 prune --keep-last 1" "$BARESNAP" prune "$REPO59" --keep-last 1
    
    # Full post-prune consistency analysis framework loop
    audit_post_prune "$REPO59" "59.4 post-prune"
    
    # Final repository verification check
    assert_ok "59.5 verify after prune" "$BARESNAP" verify "$REPO59"
    
    # TUI Info module must be consistent and functional after physical deletions
    INFO59=$("$BARESNAP" info "$REPO59" 2>&1)
    INFO_RC59=$?
    assert_eq "59.6 info works after prune" "0" "$INFO_RC59"
    
    # Verify there are no unrecognized or drifting floating metadata layers on disk
    PACKS59=$(find "$REPO59/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | wc -l)
    IDX59=$(find "$REPO59/index" -maxdepth 1 -name '*.idx' -type f 2>/dev/null | wc -l)
    log "  [INFO] Post-prune state: $PACKS59 packs, $IDX59 idx"
    
    # Confirm that exactly 1 clean snapshot survives the purge cycle
    SNAPS59_AFTER=$(get_snap_count "$REPO59")
    assert_eq "59.7 1 snapshot after prune" "1" "$SNAPS59_AFTER"


