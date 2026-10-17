# ============================================================
# 108. FIX O1: POSIX RAM Guardian — ZSTD Ultra under low memory
# ============================================================
section "108. FIX O1: POSIX RAM Guardian (ZSTD Ultra with low memory)"
REPO108="$WORK/repo108"
SRC108="$WORK/src108"
OUT108="$WORK/out108"
BASE108="$OUT108/$(basename "$SRC108")"
assert_ok "108.1 init" "$BARESNAP" init "$REPO108"
mkdir -p "$SRC108"
for i in $(seq 1 2000); do
echo "RAM guardian compressible payload line $i"
done > "$SRC108/logs.txt"
# Active branch: 100 MB simulated + Ultra level 22 => must degrade to 19
set +e
GUARD108=$(env BRS_CREATE_COMPRESSION=zstd BRS_CREATE_ZSTD_LEVEL=22 \
BRS_RAM_GUARD_FAKE_MB=100 \
"$BARESNAP" create "$REPO108" "$SRC108" 2>&1)
GUARD108_RC=$?
set -e
assert_eq "108.2 create with level 22 + simulated low RAM finishes OK" "0" "$GUARD108_RC"
if printf '%s
' "$GUARD108" | grep -q "RAM-GUARD"; then
pass "108.3 guardian activated: ZSTD level dynamically degraded"
else
fail "108.3 guardian did NOT activate with 100 MB simulated"
fi
assert_ok "108.4 verify of snapshot created under guardian" "$BARESNAP" verify "$REPO108"
SNAP108=$(get_latest_snap "$REPO108")
assert_ok "108.5 restore under guardian" "$BARESNAP" restore "$REPO108" "$SNAP108" "$OUT108"
assert_ok "108.6 content byte-identical" cmp -s "$SRC108/logs.txt" "$BASE108/logs.txt"
# Inactive branch: without simulated memory it must not degrade (if the host has RAM)
set +e
NORM108=$(env BRS_CREATE_COMPRESSION=zstd BRS_CREATE_ZSTD_LEVEL=22 \
"$BARESNAP" create "$REPO108" "$SRC108" 2>&1)
NORM108_RC=$?
set -e
assert_eq "108.7 normal ZSTD-22 create finishes OK" "0" "$NORM108_RC"
MEMAVAIL108=$(awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null || echo 999999999)
if [ "$MEMAVAIL108" -gt 716800 ]; then
if printf '%s
' "$NORM108" | grep -q "RAM-GUARD"; then
fail "108.8 guardian falsely activated with sufficient RAM"
else
pass "108.8 guardian inactive with sufficient RAM ($((MEMAVAIL108 / 1024)) MB)"
fi
else
log "  ${YELLOW}[INFO]${NC} host with low RAM ($((MEMAVAIL108 / 1024)) MB); inactive branch omitted"
fi

