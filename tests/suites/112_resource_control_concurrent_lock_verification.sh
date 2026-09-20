# ============================================================
# 112. Resource Control: Concurrent Lock Verification
# ============================================================
section "112. Resource Control: Concurrent Lock Verification"
DIR_REPO112="$WORK/repo112"
DIR_SRC112="$WORK/src112"
FICHERO_LOCK="$DIR_REPO112/.lock"
assert_ok "112.1 Initialize repository for concurrency" "$BARESNAP" init "$DIR_REPO112"
mkdir -p "$DIR_SRC112"
echo "stable control data" > "$DIR_SRC112/fichero.txt"
assert_ok "112.2 Create initial reference snapshot" "$BARESNAP" create "$DIR_REPO112" "$DIR_SRC112" "snap_base_112"
# Deterministic version:
# - The test itself retains the lock while executing create/prune.
# - No background processes with sleep that could die at the limit.
# - It is verified that a second descriptor CANNOT acquire the lock.
LOCK_112_OK=0
# We open in append mode to avoid truncating possible lock metadata.
exec 9>>"$FICHERO_LOCK"
if flock -n 9; then
log " [INFO] Occupying mutual exclusion descriptor to simulate contention..."
# Hard verification: we try to acquire the lock from a new descriptor.
# If this second attempt gets the lock, the contention is not real.
if ! ( exec 8<>"$FICHERO_LOCK"; flock -n 8 ) 2>/dev/null; then
LOCK_112_OK=1
fi
if [ "$LOCK_112_OK" -eq 1 ]; then
assert_fail "112.3 Create operation aborts cleanly when resource is occupied" \
"$BARESNAP" create "$DIR_REPO112" "$DIR_SRC112" "snap_bloqueado"
assert_fail "112.4 Prune operation aborts cleanly when resource is occupied" \
"$BARESNAP" prune "$DIR_REPO112" --keep-last 1
else
fail "112.3 Could not verify lock contention"
fail "112.4 skip"
fi
flock -u 9
log " [INFO] Mutual exclusion descriptor released correctly."
else
fail "112.3 Could not acquire test descriptor in Bash"
fail "112.4 skip"
fi
exec 9>&-
# Check that the lock was actually freed before continuing.
if ! ( exec 8<>"$FICHERO_LOCK"; flock -n 8 ) 2>/dev/null; then
log " [WARN] The lock was not freed immediately; continuing anyway."
fi
# Validate self-healing: the repository must become operational again immediately.
assert_ok "112.5 Verify operation confirms integrity after release" "$BARESNAP" verify "$DIR_REPO112"
assert_ok "112.6 Create new snapshot after recovering mutual exclusion" "$BARESNAP" create "$DIR_REPO112" "$DIR_SRC112" "snap_final_112"
rm -rf -- "$DIR_SRC112"

