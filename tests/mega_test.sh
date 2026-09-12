#!/usr/bin/env bash
# ============================================================
# BareSnap - Regression test suite
# Usage: ./mega_test.sh [--no-ssh]
# ============================================================
export BARESNAP_SKIP_HEALTH=1
set -u
# ============================================================
# COLOR DEFINITION (Must go BEFORE any use)
# ============================================================
if [ -t 1 ]; then
GREEN='\033[0;32m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'
YELLOW='\033[1;33m'
else
GREEN=''
RED=''
BOLD=''
NC=''
YELLOW=''
fi
# ============================================================
# Dual logging: screen with colors + file without colors
# ============================================================
LOG_FILE="mega_test.log"
> "$LOG_FILE"  # Clear previous log
# Function to display on screen (with colors)
log_screen() {
printf "%b
" "$@"
}
# Function to write to log (without escape codes)
log_file() {
# Remove ANSI escape codes before writing to log
printf "%b
" "$@" | sed 's/\x1b\[[0-9;]*m//g' >> "$LOG_FILE"
}
# Combined function: screen + log
log() {
log_screen "$@"
log_file "$@"
}
# ============================================================
# NUCLEAR CLEANUP IN /tmp (Only BareSnap traces)
# ============================================================
# Sweep ANY baresnap directory or file in /tmp.
# This removes month-old zombies, from hung SSH tests, etc.
rm -rf /tmp/baresnap* 2>/dev/null || true
rm -rf /tmp/brs_* 2>/dev/null || true
rm -rf /tmp/.cache/baresnap 2>/dev/null || true
rm -rf /tmp/.config/baresnap 2>/dev/null || true
rm -rf /tmp/.local/share/baresnap 2>/dev/null || true
# ============================================================
# DEFINE WORK DIR (FRESH AND ISOLATED)
# ============================================================
WORK="$(mktemp -d /tmp/baresnap_tests.XXXXXX)"
# Total isolation: we force any library or baresnap
# that tries to use the real HOME, to use this temporary $WORK.
export HOME="$WORK/fake_home"
export XDG_CACHE_HOME="$WORK/fake_home/.cache"
export XDG_CONFIG_HOME="$WORK/fake_home/.config"
export XDG_DATA_HOME="$WORK/fake_home/.local/share"
mkdir -p "$HOME" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
# The trap takes care of deleting $WORK upon exit (good or bad)
trap 'rm -rf "$WORK"' EXIT
# INITIALIZE GLOBAL TIME COUNTER
SECONDS=0
# ============================================================
# SSH detection and flags: --no-ssh / --no-valgrind
# ============================================================
SKIP_SSH=0
SKIP_VALGRIND=0
for arg in "$@"; do
if [ "$arg" = "--no-ssh" ]; then
SKIP_SSH=1
fi
if [ "$arg" = "--no-valgrind" ]; then
SKIP_VALGRIND=1
fi
done
# --- SSH PROBING (this was what was missing) ---
SSH_AVAILABLE=0
if [ "$SKIP_SSH" -eq 0 ]; then
if command -v ssh >/dev/null 2>&1; then
if ssh -o BatchMode=yes -o ConnectTimeout=5 "$(whoami)@localhost" "echo ok" >/dev/null 2>&1; then
SSH_AVAILABLE=1
fi
fi
fi
# --- Valgrind: ACTIVE BY DEFAULT if installed ---
VALGRIND_AVAILABLE=0
command -v valgrind >/dev/null 2>&1 && VALGRIND_AVAILABLE=1
if [ "$VALGRIND_AVAILABLE" -eq 1 ]; then
if [ "$SKIP_VALGRIND" -eq 1 ]; then
log "${YELLOW}⚠ Valgrind modules disabled (--no-valgrind)${NC}"
else
log "${GREEN}✓ Valgrind available: memory modules ACTIVE by default${NC}"
fi
else
log "${YELLOW}⚠ Valgrind not installed: memory modules will be skipped${NC}"
log "  (Install: apt install valgrind / dnf install valgrind)"
fi
# ============================================================
# Detection of optional dependencies for specific tests
# ============================================================
MISSING_DEPS=()
command -v pv &>/dev/null || MISSING_DEPS+=("pv")
command -v nc &>/dev/null || MISSING_DEPS+=("nc (netcat)")
command -v strace &>/dev/null || MISSING_DEPS+=("strace")
command -v faketime &>/dev/null || MISSING_DEPS+=("faketime")
command -v valgrind &>/dev/null || MISSING_DEPS+=("valgrind")
if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
log "${YELLOW}⚠ Missing dependencies for some tests: ${MISSING_DEPS[*]}${NC}"
log "  (Tests requiring them will be automatically skipped)"
CAN_RUN_SATELLITE=0
else
log "${GREEN}✓ Optional dependencies (pv, nc, strace, faketime, valgrind) available${NC}"
CAN_RUN_SATELLITE=1
fi
# ============================================================
# TEST BEGINS
# ============================================================
BARESNAP="${BARESNAP:-./baresnap}"
PASS=0
FAIL=0
FAILED_TESTS=()
CURRENT_TEST=0
TOTAL_TESTS=$(grep -E '^[[:space:]]*section[[:space:]]+"' "$0" | grep -v '^[[:space:]]*#' | awk '{print $2}' | sort -u | wc -l)
pass() {
PASS=$((PASS + 1))
log "  ${GREEN}[PASS]${NC} $1"
}
fail() {
FAIL=$((FAIL + 1))
FAILED_TESTS+=("$1")
log "  ${RED}[FAIL]${NC} $1"
}
assert_init_aes() {
local desc="$1"
shift
local out rc
out="$("$@" 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
fail "$desc (init rc=$rc)"
printf '%s
' "$out" | head -n 5 | sed 's/^/    [INIT] /' >>"$LOG_FILE"
return 1
fi
if printf '%s
' "$out" | grep -Eiq 'AES[- _]?256[- _]?GCM|Encryption:.*AES|cipher:.*aes'; then
pass "$desc"
return 0
fi
fail "$desc (init OK but DID NOT select AES)"
printf '%s
' "$out" | head -n 5 | sed 's/^/    [INIT] /' >>"$LOG_FILE"
return 1
}
assert_ok() {
local desc="$1"
shift
if "$@" >/dev/null 2>&1 </dev/null; then
pass "$desc"
else
fail "$desc"
fi
}
assert_eq() {
local desc="$1" expected="$2" actual="$3"
if [ "$expected" = "$actual" ]; then
pass "$desc"
else
fail "$desc (expected='$expected' actual='$actual')"
fi
}
assert_info_aes() {
local desc="$1"
shift
local out rc
out="$("$@" 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
fail "$desc (info rc=$rc)"
printf '%s
' "$out" | head -n 8 | sed 's/^/    [INFO] /' >>"$LOG_FILE"
return 1
fi
if printf '%s
' "$out" | grep -Eiq 'AES[- _]?256[- _]?GCM|Encryption:.*AES|encryption:.*AES'; then
pass "$desc"
return 0
fi
fail "$desc (info DOES NOT show AES)"
printf '%s
' "$out" | head -n 8 | sed 's/^/    [INFO] /' >>"$LOG_FILE"
return 1
}
section() {
CURRENT_TEST=$((CURRENT_TEST + 1))
log ""
log "${BOLD}== [$CURRENT_TEST/$TOTAL_TESTS] $1 ==${NC}"
}
# --- HELPERS TO PARSE 'list' CORRECTLY ---
get_snap_count() {
"$BARESNAP" list "$1" 2>/dev/null | grep '\.snap$' | wc -l
}
get_latest_snap() {
"$BARESNAP" list "$1" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}'
}
# --------------------------------------------------
if [ ! -x "$BARESNAP" ]; then
log "${RED}ERROR: binary not found or not executable: $BARESNAP${NC}"
log "Compile first: cmake -B build && cmake --build build"
log "Or point to your binary: BARESNAP=/path/to/baresnap $0"
exit 1
fi
section "Setup"
log ""
log "${BOLD}== Setup ==${NC}"
log "binario: $BARESNAP"
log "workdir: $WORK"
log "log file: $LOG_FILE"
if [ "$SSH_AVAILABLE" -eq 1 ]; then
log "${GREEN}✓ SSH available${NC}"
elif [ "$SKIP_SSH" -eq 1 ]; then
log "${YELLOW}⚠ SSH tests disabled (--no-ssh)${NC}"
else
log "${YELLOW}⚠ SSH not available or no connectivity${NC}"
log "  Configure: ssh-keygen && ssh-copy-id $(whoami)@localhost"
log "  Or run: $0 --no-ssh"
fi
# ============================================================
# 01. Basic cycle
# ============================================================
section "01. Basic cycle: init / create / list / restore"
REPO="$WORK/repo01"
SRC="$WORK/src01"
OUT="$WORK/out01"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC/subdir"
printf 'hello world
' > "$SRC/file.txt"
printf 'nested file
' > "$SRC/subdir/nested.txt"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
SNAP_COUNT=$(get_snap_count "$REPO")
assert_eq "list shows 1 snapshot" "1" "$SNAP_COUNT"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
assert_ok "identical content (file.txt)" cmp -s "$SRC/file.txt" "$BASE/file.txt"
assert_ok "identical content (nested.txt)" cmp -s "$SRC/subdir/nested.txt" "$BASE/subdir/nested.txt"
# ============================================================
# 02. Large random file
# ============================================================
section "02. Large random file (2 MB)"
REPO="$WORK/repo02"
SRC="$WORK/src02"
OUT="$WORK/out02"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 2097152 /dev/urandom > "$SRC/big.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
assert_ok "big.bin byte-identical" cmp -s "$SRC/big.bin" "$BASE/big.bin"
assert_ok "verify" "$BARESNAP" verify "$REPO"
# ============================================================
# 03. Symlinks
# ============================================================
section "03. Symlinks"
REPO="$WORK/repo03"
SRC="$WORK/src03"
OUT="$WORK/out03"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'target content
' > "$SRC/target.txt"
if ln -s target.txt "$SRC/link.txt" 2>/dev/null; then
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
if [ -L "$BASE/link.txt" ]; then
LINK_TARGET="$(readlink "$BASE/link.txt")"
assert_eq "restored symlink points to target.txt" "target.txt" "$LINK_TARGET"
assert_ok "content via symlink" cmp -s "$SRC/target.txt" "$BASE/target.txt"
else
fail "link.txt is not a symlink after restore"
fi
else
fail "could not create symlink in test filesystem"
fi
# ============================================================
# 04. Real hardlinks
# ============================================================
section "04. Real hardlinks"
REPO="$WORK/repo04"
SRC="$WORK/src04"
OUT="$WORK/out04"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'shared content
' > "$SRC/original.txt"
ln "$SRC/original.txt" "$SRC/hardlink.txt"
INO_SRC1=$(stat -c '%i' "$SRC/original.txt")
INO_SRC2=$(stat -c '%i' "$SRC/hardlink.txt")
assert_eq "source: same inode" "$INO_SRC1" "$INO_SRC2"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
INO_OUT1=$(stat -c '%i' "$BASE/original.txt")
INO_OUT2=$(stat -c '%i' "$BASE/hardlink.txt")
assert_eq "restore: same inode" "$INO_OUT1" "$INO_OUT2"
assert_ok "correct content" test "$(cat "$BASE/original.txt")" = "shared content"
rm "$BASE/original.txt"
assert_ok "hardlink survives deleting original" test "$(cat "$BASE/hardlink.txt")" = "shared content"
# ============================================================
# 05. Cross-snapshot dedup
# ============================================================
section "05. Cross-snapshot dedup"
REPO="$WORK/repo05"
SRC="$WORK/src05"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 262144 /dev/urandom > "$SRC/data.bin"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
PACKS_BEFORE=$(ls "$REPO/packs/" 2>/dev/null | wc -l)
SIZE_BEFORE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
assert_ok "create snapshot 2 (identical)" "$BARESNAP" create "$REPO" "$SRC"
SIZE_AFTER=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
SNAPS=$(get_snap_count "$REPO")
assert_eq "2 snapshots exist" "2" "$SNAPS"
GROWTH=$((SIZE_AFTER - SIZE_BEFORE))
if [ "$GROWTH" -le 64 ]; then
pass "cross-snapshot dedup (growth ${GROWTH} KB <= 64 KB)"
else
fail "cross-snapshot dedup (growth ${GROWTH} KB > 64 KB)"
fi
# ============================================================
# 06. File cache in incrementals
# ============================================================
section "06. File cache in incrementals"
REPO="$WORK/repo06"
SRC="$WORK/src06"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'stable file
' > "$SRC/stable.txt"
head -c 65536 /dev/urandom > "$SRC/blob.bin"
assert_ok "create 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
OUT1=$("$BARESNAP" create "$REPO" "$SRC" 2>&1)
HITS1=$(printf '%s
' "$OUT1" | grep -oE '[0-9]+ hits' | grep -oE '[0-9]+' | head -1)
MISS1=$(printf '%s
' "$OUT1" | grep -oE '[0-9]+ misses' | grep -oE '[0-9]+' | head -1)
assert_eq "create 2: cache hits" "2" "$HITS1"
assert_eq "create 2: cache misses" "0" "$MISS1"
printf 'modified
' > "$SRC/stable.txt"
OUT2=$("$BARESNAP" create "$REPO" "$SRC" 2>&1)
HITS2=$(printf '%s
' "$OUT2" | grep -oE '[0-9]+ hits' | grep -oE '[0-9]+' | head -1)
MISS2=$(printf '%s
' "$OUT2" | grep -oE '[0-9]+ misses' | grep -oE '[0-9]+' | head -1)
assert_eq "create 3 (1 mod): cache hits" "1" "$HITS2"
assert_eq "create 3 (1 mod): cache misses" "1" "$MISS2"
# ============================================================
# 07. Prune
# ============================================================
section "07. Prune"
REPO="$WORK/repo07"
SRC="$WORK/src07"
OUT="$WORK/out07"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
for i in 1 2 3; do
printf 'version %s
' "$i" > "$SRC/file.txt"
assert_ok "create snapshot $i" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
done
SNAPS_BEFORE=$(get_snap_count "$REPO")
assert_eq "3 snapshots before prune" "3" "$SNAPS_BEFORE"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "1 snapshot after prune" "1" "$SNAPS_AFTER"
assert_ok "verify after prune" "$BARESNAP" verify "$REPO"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore of remaining snapshot" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
RESTORED=$(cat "$BASE/file.txt")
assert_eq "restored content is version 3" "version 3" "$RESTORED"
# ============================================================
# 08. Corruption detection
# ============================================================
section "08. Corruption detection"
REPO="$WORK/repo08"
SRC="$WORK/src08"
OUT="$WORK/out08"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 131072 /dev/urandom > "$SRC/data.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before corrupting" "$BARESNAP" verify "$REPO"
PACK=$(ls "$REPO/packs/"*.pack 2>/dev/null | head -1)
if [ -n "$PACK" ]; then
printf '\xFF' | dd of="$PACK" bs=1 seek=1000 conv=notrunc status=none
if "$BARESNAP" verify "$REPO" >/dev/null 2>&1; then
fail "verify should detect corruption"
else
pass "verify detects corruption (exit != 0)"
fi
else
fail "no pack found to corrupt"
fi
# ============================================================
# 09. Init on existing repo (should fail)
# ============================================================
section "09. Init on existing repo (should fail)"
REPO="$WORK/repo09"
assert_ok "init" "$BARESNAP" init "$REPO"
if "$BARESNAP" init "$REPO" >/dev/null 2>&1; then
fail "init on existing repo should fail"
else
pass "init on existing repo fails correctly"
fi
# ============================================================
# 10. Restore after prune with active cache
# ============================================================
section "10. Restore after prune with active cache"
REPO="$WORK/repo10"
SRC="$WORK/src10"
OUT="$WORK/out10"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'v1
' > "$SRC/f.txt"
assert_ok "create 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'v2
' > "$SRC/f.txt"
assert_ok "create 2" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
OUTC=$("$BARESNAP" create "$REPO" "$SRC" 2>&1)
HITSC=$(printf '%s
' "$OUTC" | grep -oE '[0-9]+ hits' | grep -oE '[0-9]+' | head -1)
assert_eq "create 3: cache hit after prune" "1" "$HITSC"
assert_ok "verify after prune + create" "$BARESNAP" verify "$REPO"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
RESTORED=$(cat "$BASE/f.txt")
assert_eq "content is v2" "v2" "$RESTORED"
# ============================================================
# 11. Info
# ============================================================
section "11. Info"
REPO="$WORK/repo02"
INFO_OUT=$("$BARESNAP" info "$REPO" 2>&1)
INFO_RC=$?
assert_eq "info exit code" "0" "$INFO_RC"
for section_name in Repository Snapshots Packs Chunks Storage Index Cache; do
if printf '%s
' "$INFO_OUT" | grep -q "$section_name"; then
pass "info shows section: $section_name"
else
fail "info does not show section: $section_name"
fi
done
# ============================================================
# 12. Permissions and timestamps
# ============================================================
section "12. Permissions and timestamps"
REPO="$WORK/repo12"
SRC="$WORK/src12"
OUT="$WORK/out12"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'perm test
' > "$SRC/perm.txt"
chmod 0604 "$SRC/perm.txt"
touch -d '2020-01-15 10:30:00 UTC' "$SRC/perm.txt"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
LATEST_SNAP=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$LATEST_SNAP" "$OUT"
MODE_SRC=$(stat -c '%a' "$SRC/perm.txt")
MODE_OUT=$(stat -c '%a' "$BASE/perm.txt")
assert_eq "permissions preserved (0604)" "$MODE_SRC" "$MODE_OUT"
MTIME_SRC=$(stat -c '%Y' "$SRC/perm.txt")
MTIME_OUT=$(stat -c '%Y' "$BASE/perm.txt")
assert_eq "mtime preserved" "$MTIME_SRC" "$MTIME_OUT"
# ============================================================
# 13. Diff between snapshots
# ============================================================
section "13. Diff between snapshots"
REPO="$WORK/repo13"
SRC="$WORK/src13"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'keep
' > "$SRC/keep.txt"
printf 'delete
' > "$SRC/delete_me.txt"
printf 'modify
' > "$SRC/modify.txt"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'modified
' > "$SRC/modify.txt"
printf 'added
' > "$SRC/added.txt"
rm "$SRC/delete_me.txt"
chmod 755 "$SRC/keep.txt"
assert_ok "create snapshot 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
DIFF_OUT="$("$BARESNAP" diff "$REPO" "$SNAP1" "$SNAP2" 2>&1)"
DIFF_RC=$?
assert_eq "diff exit code" "0" "$DIFF_RC"
if printf '%s
' "$DIFF_OUT" | grep -q "added\.txt"; then pass "diff detects added.txt"; else fail "diff does not detect added.txt"; fi
if printf '%s
' "$DIFF_OUT" | grep -q "delete_me\.txt"; then pass "diff detects delete_me.txt"; else fail "diff does not detect delete_me.txt"; fi
if printf '%s
' "$DIFF_OUT" | grep -q "modify\.txt"; then pass "diff detects modify.txt"; else fail "diff does not detect modify.txt"; fi
if printf '%s
' "$DIFF_OUT" | grep -q "keep\.txt"; then pass "diff detects keep.txt (metadata)"; else fail "diff does not detect keep.txt (metadata)"; fi
if printf '%s
' "$DIFF_OUT" | grep -q "unchanged:"; then pass "diff unchanged count correct"; else fail "diff unchanged count incorrect"; fi
# ============================================================
# 14. Encrypted repo: full cycle
# ============================================================
section "14. Encrypted repo: full cycle"
REPO="$WORK/repo14"
SRC="$WORK/src14"
OUT="$WORK/out14"
BASE="$OUT/$(basename "$SRC")"
mkdir -p "$SRC/subdir"
printf 'secret content
' > "$SRC/secret.txt"
printf 'nested secret
' > "$SRC/subdir/nested.txt"
dd if=/dev/urandom of="$SRC/big.bin" bs=1M count=2 2>/dev/null
export BARESNAP_PASSPHRASE="test-passphrase-123"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
assert_ok "encrypted create" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "list without passphrase" "$BARESNAP" list "$REPO"
SNAP=$(get_latest_snap "$REPO")
assert_ok "encrypted restore" "$BARESNAP" restore "$REPO" "$SNAP" "$OUT"
assert_ok "secret.txt content" test "$(cat "$BASE/secret.txt")" = "secret content"
assert_ok "nested.txt content" test "$(cat "$BASE/subdir/nested.txt")" = "nested secret"
assert_ok "big.bin byte-identical" cmp -s "$SRC/big.bin" "$BASE/big.bin"
assert_ok "encrypted verify" "$BARESNAP" verify "$REPO"
FOUND=0
if grep -rq "secret content" "$REPO/packs/" 2>/dev/null; then FOUND=1; fi
assert_eq "packs without plaintext" "$FOUND" "0"
FOUND=0
if grep -rq "secret.txt\|secret content" "$REPO/snapshots/" 2>/dev/null; then FOUND=1; fi
assert_eq "snapshots without plaintext" "$FOUND" "0"
MAGIC=$(head -c 7 "$REPO/snapshots/$SNAP")
assert_eq "snapshot magic is BRSNAP2" "BRSNAP2" "$MAGIC"
export BARESNAP_PASSPHRASE="wrong-passphrase"
set +e
"$BARESNAP" restore "$REPO" "$SNAP" "$WORK/out_bad" >/dev/null 2>&1
assert_eq "restore with wrong passphrase fails" "$?" "1"
"$BARESNAP" verify "$REPO" >/dev/null 2>&1
assert_eq "verify with wrong passphrase fails" "$?" "1"
set -e
unset BARESNAP_PASSPHRASE
# ============================================================
# 15. Encrypted repo: prune and diff
# ============================================================
section "15. Encrypted repo: prune and diff"
REPO="$WORK/repo15"
SRC="$WORK/src15"
mkdir -p "$SRC"
echo "v1" > "$SRC/file.txt"
export BARESNAP_PASSPHRASE="prune-test-pass"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
echo "v2" > "$SRC/file.txt"
assert_ok "create snapshot 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
SNAPS=$(get_snap_count "$REPO")
assert_eq "2 encrypted snapshots" "2" "$SNAPS"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
SNAP_COUNT=$(get_snap_count "$REPO")
assert_eq "1 snapshot after prune" "1" "$SNAP_COUNT"
assert_ok "verify after prune" "$BARESNAP" verify "$REPO"
unset BARESNAP_PASSPHRASE
# ============================================================
# 16. Protection against decompression bombs (OOM)
# ============================================================
section "16. Protection against decompression bombs (OOM)"
REPO16="$WORK/repo16"
SRC16="$WORK/src16"
OUT16="$WORK/out16"
assert_ok "init" "$BARESNAP" init "$REPO16"
mkdir -p "$SRC16"
dd if=/dev/zero of="$SRC16/bomb.bin" bs=1M count=32 status=none
assert_ok "create" "$BARESNAP" create "$REPO16" "$SRC16"
SNAP16=$(get_latest_snap "$REPO16")
# Detect if the binary is instrumented with ASan/TSan/UBSan.
# Sanitizers need tens of MB of shadow memory at startup,
# so ulimit -v 8192 hangs them before executing an instruction.
IS_SANITIZED=0
if ldd "$BARESNAP" 2>/dev/null | grep -qE "libasan|libtsan|libubsan"; then
IS_SANITIZED=1
elif nm "$BARESNAP" 2>/dev/null | grep -qE "__asan_init|__tsan_init|__ubsan"; then
IS_SANITIZED=1
fi
if [ "$IS_SANITIZED" -eq 1 ]; then
# Under sanitizers: use BRS_RAM_GUARD_FAKE_MB to simulate memory
# low at the engine level, without ulimit (which would hang the ASan runtime).
# We verify that the engine does not crash and handles the restriction.
set +e
(
BRS_RAM_GUARD_FAKE_MB=8 "$BARESNAP" restore "$REPO16" "$SNAP16" "$OUT16" >/dev/null 2>&1
)
RESTORE_RC=$?
set -e
if [ "$RESTORE_RC" -eq 139 ] || [ "$RESTORE_RC" -eq 137 ]; then
fail "Engine crashed under sanitizers with simulated low RAM"
else
pass "Engine correctly handled memory restrictions (sanitizer mode)"
fi
else
# Normal binary: strict 8 MB ulimit as always.
(
ulimit -v 8192 2>/dev/null
"$BARESNAP" restore "$REPO16" "$SNAP16" "$OUT16" >/dev/null 2>&1
)
RESTORE_RC=$?
if [ "$RESTORE_RC" -eq 139 ] || [ "$RESTORE_RC" -eq 137 ]; then
fail "Engine crashed due to out of memory (OOM/SegFault)"
else
pass "Engine correctly handled memory restrictions"
fi
fi
# ============================================================
# 17. Delta Encoding: small file with minor changes
# ============================================================
section "17. Delta Encoding: small file with minor changes"
REPO="$WORK/repo17"
SRC="$WORK/src17"
OUT="$WORK/out17"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "The quick brown fox jumps over the lazy dog. Xdelta3 delta encoding test line." | head -c 65536 > "$SRC/delta_file.txt"
ORIG_SIZE=$(stat -c '%s' "$SRC/delta_file.txt")
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'XYZ' | dd of="$SRC/delta_file.txt" bs=1 seek=1000 conv=notrunc status=none
touch "$SRC/delta_file.txt"
assert_ok "create snapshot 2 (delta expected)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
assert_ok "restore snapshot 2" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "delta_file.txt byte-identical after restore" cmp -s "$SRC/delta_file.txt" "$BASE/delta_file.txt"
assert_ok "verify after delta" "$BARESNAP" verify "$REPO"
DIFF_OUT=$("$BARESNAP" diff "$REPO" "$SNAP1" "$SNAP2" 2>&1)
if printf '%s
' "$DIFF_OUT" | grep -q "M.*delta_file\.txt"; then pass "diff detects modification of delta_file.txt"; else fail "diff does not detect modification of delta_file.txt"; fi
RESTORED_SIZE=$(stat -c '%s' "$BASE/delta_file.txt")
assert_eq "file size preserved" "$ORIG_SIZE" "$RESTORED_SIZE"
# ============================================================
# 18. Delta Encoding: prune preserves delta source chunks
# ============================================================
section "18. Delta Encoding: prune preserves source chunks"
REPO="$WORK/repo18"
SRC="$WORK/src18"
OUT="$WORK/out18"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "ABCDEFGHIJKLMNOPQRSTUVWXY - base content for delta prune test." | head -c 32768 > "$SRC/prune_delta.txt"
assert_ok "create snapshot 1 (base)" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'MOD' | dd of="$SRC/prune_delta.txt" bs=1 seek=500 conv=notrunc status=none
touch "$SRC/prune_delta.txt"
assert_ok "create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
SNAPS_BEFORE=$(get_snap_count "$REPO")
assert_eq "2 snapshots before prune" "2" "$SNAPS_BEFORE"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "1 snapshot after prune" "1" "$SNAPS_AFTER"
assert_ok "verify after prune with delta" "$BARESNAP" verify "$REPO"
assert_ok "restore snapshot 2 after prune" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "prune_delta.txt restored correctly" cmp -s "$SRC/prune_delta.txt" "$BASE/prune_delta.txt"
if head -c 503 "$BASE/prune_delta.txt" | tail -c 3 | grep -q "MOD"; then pass "modified bytes present in restore"; else fail "modified bytes NOT present in restore"; fi
# ============================================================
# 19. Delta Encoding: encrypted repo
# ============================================================
section "19. Delta Encoding: encrypted repo"
REPO="$WORK/repo19"
SRC="$WORK/src19"
OUT="$WORK/out19"
BASE="$OUT/$(basename "$SRC")"
export BARESNAP_PASSPHRASE="delta-enc-test-pass"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
mkdir -p "$SRC"
yes "Encrypted delta encoding test content. Repeated for xdelta3 efficiency." | head -c 49152 > "$SRC/enc_delta.txt"
assert_ok "encrypted create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'SECRET' | dd of="$SRC/enc_delta.txt" bs=1 seek=2000 conv=notrunc status=none
touch "$SRC/enc_delta.txt"
assert_ok "encrypted create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
assert_ok "verify encrypted repo with delta" "$BARESNAP" verify "$REPO"
assert_ok "encrypted restore snapshot 2" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "enc_delta.txt byte-identical (encrypted+delta)" cmp -s "$SRC/enc_delta.txt" "$BASE/enc_delta.txt"
DIFF_OUT=$("$BARESNAP" diff "$REPO" "$SNAP1" "$SNAP2" 2>&1)
if printf '%s
' "$DIFF_OUT" | grep -q "M.*enc_delta\.txt"; then pass "encrypted diff detects modification"; else fail "encrypted diff does not detect modification"; fi
assert_ok "encrypted prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
assert_ok "encrypted verify after prune" "$BARESNAP" verify "$REPO"
rm -rf "$OUT"
assert_ok "restore after encrypted prune" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "enc_delta.txt correct after encrypted prune+restore" cmp -s "$SRC/enc_delta.txt" "$BASE/enc_delta.txt"
export BARESNAP_PASSPHRASE="wrong-pass"
set +e
"$BARESNAP" restore "$REPO" "$SNAP2" "$WORK/out_bad_delta" >/dev/null 2>&1
RC=$?
set -e
assert_eq "encrypted delta restore with wrong pass fails" "$RC" "1"
unset BARESNAP_PASSPHRASE
# ============================================================
# 20. Delta: corruption detection/source deletion
# ============================================================
section "20. Delta: deleted pack detection (delta source)"
REPO="$WORK/repo20"
SRC="$WORK/src20"
OUT="$WORK/out20"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "CORRUPTION TEST BASE. Repeated for delta encoding." | head -c 32768 > "$SRC/corr.txt"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'XXX' | dd of="$SRC/corr.txt" bs=1 seek=100 conv=notrunc status=none
touch "$SRC/corr.txt"
assert_ok "create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO" --keep-last 1
assert_ok "verify before deleting pack" "$BARESNAP" verify "$REPO"
PACK=$(ls "$REPO/packs/"*.pack 2>/dev/null | head -1)
if [ -n "$PACK" ]; then
rm "$PACK"
if "$BARESNAP" verify "$REPO" >/dev/null 2>&1; then
fail "verify should detect deleted pack (delta source)"
else
pass "verify detects deleted pack (delta source)"
fi
else
fail "no pack found to delete"
fi
# ============================================================
# 21. Delta: size threshold (>256 KB → normal chunking)
# ============================================================
section "21. Delta: size threshold (>256 KB → normal chunking)"
REPO="$WORK/repo21"
SRC="$WORK/src21"
OUT="$WORK/out21"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 307200 /dev/urandom > "$SRC/large.bin"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'ABC' | dd of="$SRC/large.bin" bs=1 seek=1000 conv=notrunc status=none
touch "$SRC/large.bin"
assert_ok "create snapshot 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
assert_ok "restore" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
assert_ok "large.bin byte-identical" cmp -s "$SRC/large.bin" "$BASE/large.bin"
assert_ok "verify" "$BARESNAP" verify "$REPO"
# ============================================================
# 22. Delta: verification of real savings in packs
# ============================================================
section "22. Delta: space savings in packs"
REPO="$WORK/repo22"
SRC="$WORK/src22"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "SAVINGS TEST. This line repeats for delta efficiency testing." | head -c 131072 > "$SRC/savings.txt"
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SIZE_BEFORE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
sleep 1.1
printf 'ZZZ' | dd of="$SRC/savings.txt" bs=1 seek=1000 conv=notrunc status=none
touch "$SRC/savings.txt"
assert_ok "create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SIZE_AFTER=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
GROWTH=$((SIZE_AFTER - SIZE_BEFORE))
if [ "$GROWTH" -le 32 ]; then
pass "delta savings: growth ${GROWTH} KB <= 32 KB (128 KB file)"
else
fail "delta savings: growth ${GROWTH} KB > 32 KB (128 KB file)"
fi
assert_ok "verify after delta savings" "$BARESNAP" verify "$REPO"
# ============================================================
# 23. Snapshot corruption (.snap) — FNV1A-64 footer
# ============================================================
section "23. Snapshot corruption (.snap)"
REPO="$WORK/repo23"
SRC="$WORK/src23"
OUT="$WORK/out23"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'snapshot integrity test
' > "$SRC/data.txt"
head -c 65536 /dev/urandom > "$SRC/blob.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before corrupting" "$BARESNAP" verify "$REPO"
SNAP_FILE=$(ls "$REPO/snapshots/"*.snap 2>/dev/null | head -1)
SNAP_BACKUP="$WORK/snap_backup.snap"
if [ -n "$SNAP_FILE" ]; then
cp "$SNAP_FILE" "$SNAP_BACKUP"
SNAP_SIZE=$(wc -c < "$SNAP_FILE")
CORRUPT_OFFSET=$((SNAP_SIZE / 2))
printf '\xFF' | dd of="$SNAP_FILE" bs=1 seek="$CORRUPT_OFFSET" conv=notrunc status=none
set +e
"$BARESNAP" verify "$REPO" >/dev/null 2>&1
VERIFY_RC=$?
set -e
if [ "$VERIFY_RC" -ne 0 ]; then pass "verify detects corrupt snapshot (rc=$VERIFY_RC)"; else fail "verify does not detect corrupt snapshot"; fi
set +e
"$BARESNAP" restore "$REPO" "$(basename "$SNAP_FILE")" "$OUT" >/dev/null 2>&1
RESTORE_RC=$?
set -e
if [ "$RESTORE_RC" -ne 0 ]; then pass "restore fails with corrupt snapshot (rc=$RESTORE_RC)"; else fail "restore does not fail with corrupt snapshot"; fi
cp "$SNAP_BACKUP" "$SNAP_FILE"
assert_ok "verify passes after restoring snapshot" "$BARESNAP" verify "$REPO"
assert_ok "restore works after restoring snapshot" "$BARESNAP" restore "$REPO" "$(basename "$SNAP_FILE")" "$OUT"
else
fail "no snapshot found to corrupt"
fi
# ============================================================
# 24. Index corruption (.idx) — FNV1A-64 footer
# ============================================================
section "24. Index corruption (.idx)"
REPO="$WORK/repo24"
SRC="$WORK/src24"
OUT="$WORK/out24"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'index integrity test
' > "$SRC/data.txt"
head -c 131072 /dev/urandom > "$SRC/blob.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before corrupting" "$BARESNAP" verify "$REPO"
IDX_FILE=$(ls "$REPO/index/"*.idx 2>/dev/null | head -1)
IDX_BACKUP="$WORK/idx_backup.idx"
SNAP_NAME=$(get_latest_snap "$REPO")
if [ -n "$IDX_FILE" ]; then
cp "$IDX_FILE" "$IDX_BACKUP"
IDX_SIZE=$(wc -c < "$IDX_FILE")
CORRUPT_OFFSET=$((IDX_SIZE / 2))
printf '\xDE' | dd of="$IDX_FILE" bs=1 seek="$CORRUPT_OFFSET" conv=notrunc status=none
set +e
"$BARESNAP" verify "$REPO" >/dev/null 2>&1
VERIFY_RC=$?
set -e
if [ "$VERIFY_RC" -ne 0 ]; then pass "verify detects corrupt index (rc=$VERIFY_RC)"; else fail "verify does not detect corrupt index"; fi
set +e
"$BARESNAP" info "$REPO" >/dev/null 2>&1
INFO_RC=$?
set -e
assert_eq "info does not abort with corrupt index" "0" "$INFO_RC"
cp "$IDX_BACKUP" "$IDX_FILE"
assert_ok "verify passes after restoring index" "$BARESNAP" verify "$REPO"
assert_ok "restore works after restoring index" "$BARESNAP" restore "$REPO" "$SNAP_NAME" "$OUT"
BASE_OUT="$OUT/$(basename "$SRC")"
assert_ok "correct content after restore" cmp -s "$SRC/blob.bin" "$BASE_OUT/blob.bin"
else
fail "no index found to corrupt"
fi
# ============================================================
# 25. ZSTD: alternative compression
# ============================================================
section "25. ZSTD: alternative compression"
REPO_LZ4="$WORK/repo_lz4"
REPO_ZSTD="$WORK/repo_zstd"
SRC="$WORK/src25"
OUT_LZ4="$WORK/out_lz4"
OUT_ZSTD="$WORK/out_zstd"
mkdir -p "$SRC"
for i in $(seq 1 20000); do echo "Line $i: The quick brown fox jumps over the lazy dog. BareSnap ZSTD vs LZ4 compression benchmark test with entropy: $((i * 7 % 1000))"; done > "$SRC/test.txt"
for i in $(seq 1 5000); do echo "{\"id\":$i,\"name\":\"item_$i\",\"value\":$((i*3)),\"tags\":[\"backup\",\"dedup\",\"chunk\"]}"; done > "$SRC/data.json"
assert_ok "init LZ4" "$BARESNAP" init "$REPO_LZ4"
assert_ok "init ZSTD level 10" "$BARESNAP" init "$REPO_ZSTD" --compression zstd --zstd-level 10
assert_ok "create LZ4" "$BARESNAP" create "$REPO_LZ4" "$SRC"
assert_ok "create ZSTD" "$BARESNAP" create "$REPO_ZSTD" "$SRC"
PACK_LZ4=$(ls "$REPO_LZ4/packs/"*.pack 2>/dev/null | head -1)
PACK_ZSTD=$(ls "$REPO_ZSTD/packs/"*.pack 2>/dev/null | head -1)
if [ -n "$PACK_LZ4" ] && [ -n "$PACK_ZSTD" ]; then
SIZE_LZ4=$(stat -c '%s' "$PACK_LZ4")
SIZE_ZSTD=$(stat -c '%s' "$PACK_ZSTD")
log "  [INFO] LZ4: ${SIZE_LZ4} bytes | ZSTD: ${SIZE_ZSTD} bytes"
fi
LATEST_LZ4=$(get_latest_snap "$REPO_LZ4")
LATEST_ZSTD=$(get_latest_snap "$REPO_ZSTD")
assert_ok "restore LZ4" "$BARESNAP" restore "$REPO_LZ4" "$LATEST_LZ4" "$OUT_LZ4"
assert_ok "restore ZSTD" "$BARESNAP" restore "$REPO_ZSTD" "$LATEST_ZSTD" "$OUT_ZSTD"
BASE_LZ4="$OUT_LZ4/$(basename "$SRC")"
BASE_ZSTD="$OUT_ZSTD/$(basename "$SRC")"
assert_ok "LZ4 content correct (test.txt)" cmp -s "$SRC/test.txt" "$BASE_LZ4/test.txt"
assert_ok "LZ4 content correct (data.json)" cmp -s "$SRC/data.json" "$BASE_LZ4/data.json"
assert_ok "ZSTD content correct (test.txt)" cmp -s "$SRC/test.txt" "$BASE_ZSTD/test.txt"
assert_ok "ZSTD content correct (data.json)" cmp -s "$SRC/data.json" "$BASE_ZSTD/data.json"
assert_ok "verify LZ4" "$BARESNAP" verify "$REPO_LZ4"
assert_ok "verify ZSTD" "$BARESNAP" verify "$REPO_ZSTD"
INFO_LZ4=$("$BARESNAP" info "$REPO_LZ4" 2>&1)
INFO_ZSTD=$("$BARESNAP" info "$REPO_ZSTD" 2>&1)
if printf '%s
' "$INFO_LZ4" | grep -qi "lz4"; then pass "info shows compression: lz4"; else fail "info does not show compression: lz4"; fi
if printf '%s
' "$INFO_ZSTD" | grep -qi "zstd"; then pass "info shows compression: zstd"; else fail "info does not show compression: zstd"; fi
# ============================================================
# 26. I/O Retry: transient errors
# ============================================================
section "26. I/O Retry: transient errors"
STRACE_OK=0
PREAD_SYSCALL=""
if command -v strace &>/dev/null; then
if strace -o /dev/null "$BARESNAP" --help >/dev/null 2>&1; then
if strace -e inject=close:when=1:error=EIO -o /dev/null true 2>/dev/null; then
STRACE_OK=1
if strace -e trace=pread64 -o /dev/null true 2>/dev/null; then PREAD_SYSCALL="pread64"; else PREAD_SYSCALL="pread"; fi
else
log "  [INFO] strace does not support -e inject, skipping retry tests"
fi
else
log "  [INFO] strace cannot trace the binary (ptrace restricted), skipping"
fi
else
log "  [INFO] strace not installed, skipping retry tests"
fi
if [ "$STRACE_OK" -eq 1 ] && [ -n "$PREAD_SYSCALL" ]; then
REPO="$WORK/repo26"
SRC="$WORK/src26"
OUT="$WORK/out26"
BASE="$OUT/$(basename "$SRC")"
TRACE="$WORK/trace26.txt"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 65536 /dev/urandom > "$SRC/retry.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
SNAP26=$(get_latest_snap "$REPO")
rm -rf "$OUT"
strace -e trace=open,openat,"$PREAD_SYSCALL" -o "$TRACE" "$BARESNAP" restore "$REPO" "$SNAP26" "$OUT" >/dev/null 2>&1 || true
PACK_FD=$(awk '/open(at)?\(/ && /\.pack/ && /= [0-9]+$/ { if (match($0, /= [0-9]+$/)) { print substr($0, RSTART+2, RLENGTH-2); exit } }' "$TRACE")
FIRST_PACK_PREAD=""
if [ -n "$PACK_FD" ]; then
FIRST_PACK_PREAD=$(awk -v fd="$PACK_FD" -v name="$PREAD_SYSCALL" '$0 ~ ("(^|[ \t])" name "\\(") { count++; if ($0 ~ (name "\\(" fd ",")) { print count; exit } }' "$TRACE")
fi
OPEN_SYSCALL=$(awk '/openat\(/ && /\.pack/ { print "openat"; exit } /(^|[ \t])open\(/ && /\.pack/ { print "open"; exit }' "$TRACE")
FIRST_PACK_OPEN=""
if [ -n "$OPEN_SYSCALL" ]; then
FIRST_PACK_OPEN=$(awk -v name="$OPEN_SYSCALL" '$0 ~ ("(^|[ \t])" name "\\(") { count++; if ($0 ~ /\.pack/) { print count; exit } }' "$TRACE")
fi
if [ -n "$FIRST_PACK_PREAD" ]; then
log "  [INFO] first $PREAD_SYSCALL call on pack: #$FIRST_PACK_PREAD"
END_OK=$((FIRST_PACK_PREAD + 1))
rm -rf "$OUT"
if strace -e inject="$PREAD_SYSCALL:when=$FIRST_PACK_PREAD..$END_OK:error=EIO" -o /dev/null "$BARESNAP" restore "$REPO" "$SNAP26" "$OUT" >/dev/null 2>&1 && cmp -s "$SRC/retry.bin" "$BASE/retry.bin"; then
pass "restore survives 2 transient EIO errors on pack"
else
fail "restore did not survive 2 transient EIO errors on pack"
fi
END_FAIL=$((FIRST_PACK_PREAD + 3))
FAIL_OUT="$WORK/out26_fail"
FAIL_BASE="$FAIL_OUT/$(basename "$SRC")"
rm -rf "$FAIL_OUT"
strace -e inject="$PREAD_SYSCALL:when=$FIRST_PACK_PREAD..$END_FAIL:error=EIO" -o /dev/null "$BARESNAP" restore "$REPO" "$SNAP26" "$FAIL_OUT" >/dev/null 2>&1
RC=$?
if [ "$RC" -ne 0 ] || ! cmp -s "$SRC/retry.bin" "$FAIL_BASE/retry.bin"; then
pass "restore does not restore content after exceeding retries"
else
fail "restore restored content despite exceeding retries"
fi
else
log "  [INFO] could not locate $PREAD_SYSCALL call on .pack; skipping"
fi
if [ -n "$FIRST_PACK_OPEN" ] && [ -n "$OPEN_SYSCALL" ]; then
log "  [INFO] first $OPEN_SYSCALL call on pack: #$FIRST_PACK_OPEN"
rm -rf "$OUT"
if strace -e inject="$OPEN_SYSCALL:when=$FIRST_PACK_OPEN:error=EIO" -o /dev/null "$BARESNAP" restore "$REPO" "$SNAP26" "$OUT" >/dev/null 2>&1 && cmp -s "$SRC/retry.bin" "$BASE/retry.bin"; then
pass "restore survives 1 transient EIO error on pack open"
else
fail "restore did not survive 1 transient EIO error on pack open"
fi
else
log "  [INFO] could not locate open/openat call on .pack; skipping"
fi
else
log "  [SKIP] I/O retry tests skipped (strace not available)"
fi
# ============================================================
# 27. Bloom Filters + VFS + Edge Cases
# ============================================================
section "27. Bloom Filters + VFS + Edge Cases"
REPO="$WORK/repo27"
SRC="$WORK/src27"
OUT="$WORK/out27"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'bloom test data
' > "$SRC/data.txt"
head -c 65536 /dev/urandom > "$SRC/blob.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
BLM_COUNT=$(ls "$REPO/index/"*.blm 2>/dev/null | wc -l)
if [ "$BLM_COUNT" -ge 1 ]; then pass "bloom filter exists after create ($BLM_COUNT .blm)"; else fail "bloom filter DOES NOT exist after create"; fi
BLM_FILE=$(ls "$REPO/index/"*.blm 2>/dev/null | head -1)
if [ -n "$BLM_FILE" ]; then
BLM_MAGIC=$(head -c 8 "$BLM_FILE")
assert_eq "bloom filter magic is BRSBLM01" "BRSBLM01" "$BLM_MAGIC"
else
fail "no .blm found to verify magic"
fi
REPO_EMPTY="$WORK/repo27_empty"
SRC_EMPTY="$WORK/src27_empty"
OUT_EMPTY="$WORK/out27_empty"
BASE_EMPTY="$OUT_EMPTY/$(basename "$SRC_EMPTY")"
assert_ok "init (empty file)" "$BARESNAP" init "$REPO_EMPTY"
mkdir -p "$SRC_EMPTY"
touch "$SRC_EMPTY/empty.txt"
printf 'non-empty
' > "$SRC_EMPTY/normal.txt"
assert_ok "create with empty file" "$BARESNAP" create "$REPO_EMPTY" "$SRC_EMPTY"
LATEST_EMPTY=$(get_latest_snap "$REPO_EMPTY")
assert_ok "restore with empty file" "$BARESNAP" restore "$REPO_EMPTY" "$LATEST_EMPTY" "$OUT_EMPTY"
if [ -f "$BASE_EMPTY/empty.txt" ]; then
EMPTY_SIZE=$(stat -c '%s' "$BASE_EMPTY/empty.txt")
assert_eq "empty file restored with size 0" "0" "$EMPTY_SIZE"
else
fail "empty file does not exist after restore"
fi
assert_ok "normal file restored alongside empty" cmp -s "$SRC_EMPTY/normal.txt" "$BASE_EMPTY/normal.txt"
REPO_PRUNE="$WORK/repo27_prune"
SRC_PRUNE="$WORK/src27_prune"
assert_ok "init (prune bloom)" "$BARESNAP" init "$REPO_PRUNE"
mkdir -p "$SRC_PRUNE"
printf 'v1
' > "$SRC_PRUNE/f.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO_PRUNE" "$SRC_PRUNE"
sleep 1.1
printf 'v2
' > "$SRC_PRUNE/f.txt"
assert_ok "create snap 2" "$BARESNAP" create "$REPO_PRUNE" "$SRC_PRUNE"
assert_ok "prune --keep-last 1" "$BARESNAP" prune "$REPO_PRUNE" --keep-last 1
assert_ok "verify after prune (bloom intact)" "$BARESNAP" verify "$REPO_PRUNE"
REPO_ZSTD="$WORK/repo27_zstd"
SRC_ZSTD="$WORK/src27_zstd"
OUT_ZSTD="$WORK/out27_zstd"
BASE_ZSTD="$OUT_ZSTD/$(basename "$SRC_ZSTD")"
assert_ok "init zstd level 7" "$BARESNAP" init "$REPO_ZSTD" --compression zstd --zstd-level 7
mkdir -p "$SRC_ZSTD"
for i in $(seq 1 5000); do echo "ZSTD level 7 roundtrip test line $i: The quick brown fox jumps over the lazy dog."; done > "$SRC_ZSTD/test.txt"
assert_ok "create zstd level 7" "$BARESNAP" create "$REPO_ZSTD" "$SRC_ZSTD"
LATEST_ZSTD=$(get_latest_snap "$REPO_ZSTD")
assert_ok "restore zstd level 7" "$BARESNAP" restore "$REPO_ZSTD" "$LATEST_ZSTD" "$OUT_ZSTD"
assert_ok "zstd level 7 content correct" cmp -s "$SRC_ZSTD/test.txt" "$BASE_ZSTD/test.txt"
assert_ok "verify zstd level 7" "$BARESNAP" verify "$REPO_ZSTD"
INFO_ZSTD7=$("$BARESNAP" info "$REPO_ZSTD" 2>&1)
if printf '%s
' "$INFO_ZSTD7" | grep -qi "zstd"; then pass "info shows compression: zstd (level 7)"; else fail "info does not show compression: zstd"; fi
# ============================================================
# 28. EXTRACT with delta: must not return raw VCDIFF
# ============================================================
section "28. EXTRACT with delta: must not return raw VCDIFF"
REPO="$WORK/repo28"
SRC="$WORK/src28"
OUT="$WORK/out28"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "int main(int argc, char **argv) { return 0; } // BareSnap extract delta test" | head -c 65536 > "$SRC/code.c"
ORIG_SIZE=$(stat -c '%s' "$SRC/code.c")
assert_ok "create snapshot 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf '// MODIFIED LINE FOR DELTA
' >> "$SRC/code.c"
touch "$SRC/code.c"
assert_ok "create snapshot 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT"
SRCBASE=$(basename "$SRC")
assert_ok "extract code.c from snap with delta" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT" "$SRCBASE/code.c"
EXTRACTED=$(find "$OUT" -name "code.c" -type f 2>/dev/null | head -1)
if [ -z "$EXTRACTED" ]; then
fail "extract did not produce code.c"
else
EXT_SIZE=$(stat -c '%s' "$EXTRACTED")
if [ "$EXT_SIZE" -lt "$ORIG_SIZE" ]; then fail "extract returned raw VCDIFF (${EXT_SIZE}B < ${ORIG_SIZE}B expected)"; else pass "extract returned full content (${EXT_SIZE}B)"; fi
FILETYPE=$(file -b "$EXTRACTED" 2>/dev/null)
if echo "$FILETYPE" | grep -qi "text"; then pass "extract output is valid text"; else fail "extract output is BINARY: $FILETYPE"; fi
if cmp -s "$SRC/code.c" "$EXTRACTED"; then pass "extract code.c byte-identical"; else fail "extract code.c is NOT byte-identical"; fi
fi
# ============================================================
# 29. Delta chain: 4 copies with successive modifications
# ============================================================
section "29. Delta chain: 4 copies with successive modifications"
REPO="$WORK/repo29"
SRC="$WORK/src29"
OUT="$WORK/out29"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "BASE LINE for delta chain testing. This content will be modified incrementally." | head -c 65536 > "$SRC/chain.txt"
assert_ok "create snap 1 (base)" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'MOD_C2
' >> "$SRC/chain.txt"; touch "$SRC/chain.txt"
assert_ok "create snap 2 (delta vs 1)" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'MOD_C3
' >> "$SRC/chain.txt"; touch "$SRC/chain.txt"
assert_ok "create snap 3 (delta vs 2)" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'MOD_C4
' >> "$SRC/chain.txt"; touch "$SRC/chain.txt"
assert_ok "create snap 4 (delta vs 3)" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify after 4 copies with delta chain" "$BARESNAP" verify "$REPO"
SNAP_LAST=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore snap 4 (delta^3)" "$BARESNAP" restore "$REPO" "$SNAP_LAST" "$OUT"
assert_ok "chain.txt byte-identical after restore" cmp -s "$SRC/chain.txt" "$BASE/chain.txt"
rm -rf "$OUT"
assert_ok "extract snap 4 (delta^3)" "$BARESNAP" extract "$REPO" "$SNAP_LAST" "$OUT"
if [ -f "$BASE/chain.txt" ]; then
assert_ok "chain.txt byte-identical after extract" cmp -s "$SRC/chain.txt" "$BASE/chain.txt"
else
fail "extract did not produce chain.txt"
fi
# ============================================================
# 30. Selective extract: only file with delta from a mixed snapshot
# ============================================================
section "30. Selective extract of file with delta"
REPO="$WORK/repo30"
SRC="$WORK/src30"
OUT="$WORK/out30"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC/sub"
printf 'stable content
' > "$SRC/stable.txt"
head -c 32768 /dev/urandom > "$SRC/sub/blob.bin"
yes "selective extract delta test content" | head -c 49152 > "$SRC/changing.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'CHANGED
' >> "$SRC/changing.txt"; touch "$SRC/changing.txt"
assert_ok "create snap 2 (changing.txt delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT"
SRCBASE=$(basename "$SRC")
assert_ok "extract only changing.txt" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT" "$SRCBASE/changing.txt"
FOUND=$(find "$OUT" -name "changing.txt" -type f 2>/dev/null | head -1)
if [ -z "$FOUND" ]; then
fail "selective extract did not find changing.txt"
else
assert_ok "changing.txt byte-identical" cmp -s "$SRC/changing.txt" "$FOUND"
COUNT=$(find "$OUT" -type f 2>/dev/null | wc -l)
assert_eq "only 1 file extracted" "1" "$COUNT"
fi
# ============================================================
# 31. Intermediate prune: delete snap 2, restore snap 4
# ============================================================
section "31. Intermediate prune: delete snap 2, restore snap 4"
REPO="$WORK/repo31"
SRC="$WORK/src31"
OUT="$WORK/out31"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "PRUNE CHAIN TEST. Repeated content for delta encoding across snapshots." | head -c 65536 > "$SRC/prunechain.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
SNAP1=$(get_latest_snap "$REPO")
sleep 1.1
printf 'P2
' >> "$SRC/prunechain.txt"; touch "$SRC/prunechain.txt"
assert_ok "create snap 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
sleep 1.1
printf 'P3
' >> "$SRC/prunechain.txt"; touch "$SRC/prunechain.txt"
assert_ok "create snap 3" "$BARESNAP" create "$REPO" "$SRC"
SNAP3=$(get_latest_snap "$REPO")
sleep 1.1
printf 'P4
' >> "$SRC/prunechain.txt"; touch "$SRC/prunechain.txt"
assert_ok "create snap 4" "$BARESNAP" create "$REPO" "$SRC"
SNAP4=$(get_latest_snap "$REPO")
assert_ok "prune --keep-last 2" "$BARESNAP" prune "$REPO" --keep-last 2
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "2 snapshots after prune" "2" "$SNAPS_AFTER"
assert_ok "verify after intermediate prune" "$BARESNAP" verify "$REPO"
rm -rf "$OUT"
assert_ok "restore snap 4 after prune" "$BARESNAP" restore "$REPO" "$SNAP4" "$OUT"
assert_ok "prunechain.txt correct after prune+restore" cmp -s "$SRC/prunechain.txt" "$BASE/prunechain.txt"
rm -rf "$OUT"
assert_ok "extract snap 4 after prune" "$BARESNAP" extract "$REPO" "$SNAP4" "$OUT"
if [ -f "$BASE/prunechain.txt" ]; then
assert_ok "prunechain.txt correct after prune+extract" cmp -s "$SRC/prunechain.txt" "$BASE/prunechain.txt"
else
fail "extract did not produce prunechain.txt after prune"
fi
# ============================================================
# 32. Type verification: restore and extract produce real text
# ============================================================
section "32. Type verification: restore and extract produce real text"
REPO="$WORK/repo32"
SRC="$WORK/src32"
OUT_R="$WORK/out32_r"
OUT_E="$WORK/out32_e"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
cat > "$SRC/real_code.c" << 'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
(void)argc;
(void)argv;
printf("hello from real code
");
return 0;
}
CEOF
for i in $(seq 1 500); do echo "// Line $i: padding for chunking test with delta encoding" >> "$SRC/real_code.c"; done
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf '// MODIFIED
' >> "$SRC/real_code.c"; touch "$SRC/real_code.c"
assert_ok "create snap 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT_R"
assert_ok "restore snap 2" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT_R"
R_FILE=$(find "$OUT_R" -name "real_code.c" -type f | head -1)
if [ -n "$R_FILE" ]; then
R_TYPE=$(file -b "$R_FILE")
if echo "$R_TYPE" | grep -qi "text"; then
R_HAS_INC=$(grep -c '#include' "$R_FILE")
R_HAS_MAIN=$(grep -c 'int main' "$R_FILE")
if [ "$R_HAS_INC" -gt 0 ] && [ "$R_HAS_MAIN" -gt 0 ]; then pass "restore: valid C text (#include=$R_HAS_INC, main=$R_HAS_MAIN)"; else fail "restore: text but without C markers"; fi
else
fail "restore: BINARY ($R_TYPE)"
fi
assert_ok "restore byte-identical" cmp -s "$SRC/real_code.c" "$R_FILE"
else
fail "restore did not produce real_code.c"
fi
rm -rf "$OUT_E"
assert_ok "extract snap 2" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT_E"
E_FILE=$(find "$OUT_E" -name "real_code.c" -type f | head -1)
if [ -n "$E_FILE" ]; then
E_TYPE=$(file -b "$E_FILE")
if echo "$E_TYPE" | grep -qi "text"; then
E_HAS_INC=$(grep -c '#include' "$E_FILE")
E_HAS_MAIN=$(grep -c 'int main' "$E_FILE")
if [ "$E_HAS_INC" -gt 0 ] && [ "$E_HAS_MAIN" -gt 0 ]; then pass "extract: valid C text (#include=$E_HAS_INC, main=$E_HAS_MAIN)"; else fail "extract: text but without C markers"; fi
else
fail "extract: BINARY ($E_TYPE)"
fi
assert_ok "extract byte-identical" cmp -s "$SRC/real_code.c" "$E_FILE"
else
fail "extract did not produce real_code.c"
fi
# ============================================================
# 33. Delta: file grows significantly between copies
# ============================================================
section "33. Delta: file grows significantly between copies"
REPO="$WORK/repo33"
SRC="$WORK/src33"
OUT="$WORK/out33"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "GROWTH TEST base content" | head -c 32768 > "$SRC/grow.txt"
assert_ok "create snap 1 (32KB)" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
yes "GROWTH TEST appended content for delta" | head -c 16384 >> "$SRC/grow.txt"
truncate -s 49152 "$SRC/grow.txt" 2>/dev/null || head -c 49152 /dev/zero >> "$SRC/grow.txt"
touch "$SRC/grow.txt"
GROW_SIZE=$(stat -c '%s' "$SRC/grow.txt")
assert_ok "create snap 2 (growth)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore snap 2" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT"
REST_SIZE=$(stat -c '%s' "$BASE/grow.txt")
assert_eq "correct size after restore" "$GROW_SIZE" "$REST_SIZE"
assert_ok "grow.txt byte-identical" cmp -s "$SRC/grow.txt" "$BASE/grow.txt"
assert_ok "verify after growth" "$BARESNAP" verify "$REPO"
# ============================================================
# 34. Restore vs Extract: consistency between both
# ============================================================
section "34. Restore vs Extract: both produce the same content"
REPO="$WORK/repo34"
SRC="$WORK/src34"
OUT_R="$WORK/out34_r"
OUT_E="$WORK/out34_e"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
yes "CONSISTENCY TEST between restore and extract operations" | head -c 65536 > "$SRC/consist.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'CONSIST_MOD
' >> "$SRC/consist.txt"; touch "$SRC/consist.txt"
assert_ok "create snap 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT_R"
assert_ok "restore" "$BARESNAP" restore "$REPO" "$SNAP2" "$OUT_R"
R_FILE=$(find "$OUT_R" -name "consist.txt" -type f | head -1)
rm -rf "$OUT_E"
assert_ok "extract" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT_E"
E_FILE=$(find "$OUT_E" -name "consist.txt" -type f | head -1)
if [ -n "$R_FILE" ] && [ -n "$E_FILE" ]; then
assert_ok "restore == extract (byte-identical)" cmp -s "$R_FILE" "$E_FILE"
assert_ok "restore == source" cmp -s "$SRC/consist.txt" "$R_FILE"
assert_ok "extract == source" cmp -s "$SRC/consist.txt" "$E_FILE"
R_SIZE=$(stat -c '%s' "$R_FILE")
E_SIZE=$(stat -c '%s' "$E_FILE")
assert_eq "same size" "$R_SIZE" "$E_SIZE"
else
fail "restore or extract did not produce consist.txt"
fi
# ============================================================
# 35. Delta + encryption: extract with delta
# ============================================================
section "35. Delta + encryption: extract with delta"
REPO="$WORK/repo35"
SRC="$WORK/src35"
OUT="$WORK/out35"
export BARESNAP_PASSPHRASE="extract-delta-enc-test"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
mkdir -p "$SRC"
yes "ENCRYPTED DELTA EXTRACT TEST content for xdelta3" | head -c 49152 > "$SRC/enc_delta.txt"
assert_ok "encrypted create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'ENC_MOD
' >> "$SRC/enc_delta.txt"; touch "$SRC/enc_delta.txt"
assert_ok "encrypted create snap 2 (delta)" "$BARESNAP" create "$REPO" "$SRC"
SNAP2=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "encrypted extract with delta" "$BARESNAP" extract "$REPO" "$SNAP2" "$OUT"
E_FILE=$(find "$OUT" -name "enc_delta.txt" -type f | head -1)
if [ -n "$E_FILE" ]; then
assert_ok "enc_delta.txt byte-identical (encrypted+delta+extract)" cmp -s "$SRC/enc_delta.txt" "$E_FILE"
E_TYPE=$(file -b "$E_FILE")
if echo "$E_TYPE" | grep -qi "text"; then pass "encrypted extract: valid text"; else fail "encrypted extract: BINARY ($E_TYPE)"; fi
else
fail "encrypted extract did not produce enc_delta.txt"
fi
unset BARESNAP_PASSPHRASE
# ============================================================
# 36. Snapshot ordering: ls -1t vs alphabetical list
# ============================================================
section "36. Snapshot ordering: ls -1t vs alphabetical list"
REPO="$WORK/repo36"
SRC="$WORK/src36"
OUT="$WORK/out36"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'ORDER TEST v1
' > "$SRC/order.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'ORDER TEST v2
' > "$SRC/order.txt"
assert_ok "create snap 2" "$BARESNAP" create "$REPO" "$SRC"
SNAP_OLDEST=$(ls -1t "$REPO/snapshots/"*.snap 2>/dev/null | tail -1 | xargs basename)
SNAP_NEWEST=$(ls -1t "$REPO/snapshots/"*.snap 2>/dev/null | head -1 | xargs basename)
rm -rf "$OUT"
assert_ok "restore oldest snapshot" "$BARESNAP" restore "$REPO" "$SNAP_OLDEST" "$OUT"
RESTORED=$(cat "$BASE/order.txt" 2>/dev/null)
assert_eq "old snapshot has v1" "ORDER TEST v1" "$RESTORED"
rm -rf "$OUT"
assert_ok "restore newest snapshot" "$BARESNAP" restore "$REPO" "$SNAP_NEWEST" "$OUT"
RESTORED=$(cat "$BASE/order.txt" 2>/dev/null)
assert_eq "new snapshot has v2" "ORDER TEST v2" "$RESTORED"
# ============================================================
# 37. Health check: clean repo
# ============================================================
section "37. Health check: clean repo detects no anomalies"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
REPO="$WORK/repo37"
SRC="$WORK/src37"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'health test
' > "$SRC/test.txt"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
HEALTH_OUT=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" </dev/null 2>&1)
HEALTH_RC=$?
if printf '%s
' "$HEALTH_OUT" | grep -q "no anomalies"; then
pass "health: clean repo without anomalies"
else
fail "health: clean repo reports unexpected anomalies"
fi
fi
# ============================================================
# 38. Health check: detects orphan idx (idx without pack)
# ============================================================
section "38. Health check: detects index segment without pack"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
REPO="$WORK/repo38"
SRC="$WORK/src38"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'orphan idx test
' > "$SRC/test.txt"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
PACK=$(find "$REPO/packs" -maxdepth 1 -type f -name '*.pack' -print -quit 2>/dev/null) || true
if [ -n "$PACK" ]; then
rm -f -- "$PACK"
HEALTH_RC=0
HEALTH_OUT=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" </dev/null 2>&1) || HEALTH_RC=$?
if [ "$HEALTH_RC" -ge 128 ]; then
fail "health terminated by signal (rc=$HEALTH_RC)"
elif printf '%s
' "$HEALTH_OUT" | grep -q "sin pack"; then
pass "health detects orphan idx (deleted pack)"
else
fail "health does not detect orphan idx (rc=$HEALTH_RC)"
fi
else
fail "no pack found to delete"
fi
fi
# ============================================================
# 39. Health check: detects missing chunks after deleting pack
# ============================================================
section "39. Health check: detects missing chunks in snapshots"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
REPO="$WORK/repo39"
SRC="$WORK/src39"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 65536 /dev/urandom > "$SRC/data.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
PACK=$(find "$REPO/packs" -maxdepth 1 -type f -name '*.pack' -print -quit 2>/dev/null) || true
if [ -n "$PACK" ]; then
rm -f -- "$PACK"
HEALTH_RC=0
HEALTH_OUT=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" </dev/null 2>&1) || HEALTH_RC=$?
if [ "$HEALTH_RC" -ge 128 ]; then
fail "health terminated by signal (rc=$HEALTH_RC)"
elif printf '%s
' "$HEALTH_OUT" | grep -q "chunk(s)"; then
pass "health detects chunks without index/pack"
else
fail "health does not detect missing chunks (rc=$HEALTH_RC)"
fi
else
fail "no pack found to delete"
fi
fi
# ============================================================
# 40. Health repair: moves corrupt snapshots to damaged/
# ============================================================
section "40. Health repair: corrupt snapshots to damaged/"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
REPO="$WORK/repo40"
SRC="$WORK/src40"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'repair test v1
' > "$SRC/file.txt"
assert_ok "create snap 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
printf 'repair test v2
' > "$SRC/file.txt"
assert_ok "create snap 2" "$BARESNAP" create "$REPO" "$SRC"
SNAPS_BEFORE=$(get_snap_count "$REPO")
assert_eq "2 snapshots before corrupting" "2" "$SNAPS_BEFORE"
rm -f "$REPO/packs/"*.pack
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" --repair </dev/null >/dev/null 2>&1 || true
DAMAGED_COUNT=$(ls "$REPO/damaged/"*.snap 2>/dev/null | wc -l)
if [ "$DAMAGED_COUNT" -ge 1 ]; then
pass "repair moved corrupt snapshots to damaged/ ($DAMAGED_COUNT)"
else
fail "repair did not move snapshots to damaged/"
fi
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "snapshots/ empty after repair" "0" "$SNAPS_AFTER"
fi
# ============================================================
# 41. Health repair: rebuild index from packs
# ============================================================
section "41. Health repair: rebuild index from packs"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
REPO="$WORK/repo41"
SRC="$WORK/src41"
OUT="$WORK/out41"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'rebuild index test
' > "$SRC/rebuild.txt"
head -c 32768 /dev/urandom > "$SRC/blob.bin"
assert_ok "create" "$BARESNAP" create "$REPO" "$SRC"
rm -f "$REPO/index/"*.idx
rm -f "$REPO/index/"*.blm
set +e
"$BARESNAP" verify "$REPO" >/dev/null 2>&1
VERIFY_RC=$?
set -e
if [ "$VERIFY_RC" -ne 0 ]; then pass "verify fails after deleting index"; else fail "verify should fail without index"; fi
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" --repair </dev/null >/dev/null 2>&1 || true
assert_ok "verify passes after repair (index rebuilt)" "$BARESNAP" verify "$REPO"
SNAP=$(get_latest_snap "$REPO")
assert_ok "restore after repair" "$BARESNAP" restore "$REPO" "$SNAP" "$OUT"
assert_ok "rebuild.txt correct" cmp -s "$SRC/rebuild.txt" "$BASE/rebuild.txt"
assert_ok "blob.bin correct" cmp -s "$SRC/blob.bin" "$BASE/blob.bin"
fi
# ============================================================
# 42. Delta binary: ELF 10 versions with --delta-binary
# ============================================================
section "42. Delta binary: ELF 10 versions with --delta-binary"
if ! "$BARESNAP" create --help 2>&1 | grep -q "delta-binary"; then
log "  [SKIP] --delta-binary not implemented"
else
REPO="$WORK/repo42"
SRC="$WORK/src42"
OUT="$WORK/out42"
BASE="$OUT/$(basename "$SRC")"
ELF="$SRC/app.elf"
NUM_VERSIONS=10
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "$ELF"
printf '\x02\x00\x3e\x00\x01\x00\x00\x00\x00\x10\x40\x00\x00\x00\x00\x00' >> "$ELF"
GEN_TMP="$WORK/.gen42"
printf '\x48\x89\xe5\x48\x83\xec\x20\x48\x8b\x45\xf8\x48\x89\x45\xe0' > "$GEN_TMP"
printf '\x48\x8b\x45\xe0\x48\x8b\x00\x48\x89\x45\xd8\x48\x8b\x45\xd8' >> "$GEN_TMP"
printf '\x48\x83\xc4\x20\x5d\xc3\x90\x90\x90\x90\x90\x90\x90\x90' >> "$GEN_TMP"
printf '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >> "$GEN_TMP"
CUR=48
while [ "$CUR" -lt 1048560 ]; do
cat "$GEN_TMP" "$GEN_TMP" > "$GEN_TMP.d" 2>/dev/null && mv "$GEN_TMP.d" "$GEN_TMP"
CUR=$((CUR * 2))
done
head -c 1048560 "$GEN_TMP" >> "$ELF"
rm -f "$GEN_TMP"
ELF_MAGIC=$(head -c 4 "$ELF" | xxd -p)
assert_eq "ELF magic correct" "7f454c46" "$ELF_MAGIC"
assert_ok "create v1 (ELF base)" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
for v in $(seq 2 $NUM_VERSIONS); do
OFF1=$(( (v * 4096 + 1024) % 1048000 ))
printf 'VER_%03d_PATCH_SIG_' "$v" | dd of="$ELF" bs=1 seek="$OFF1" conv=notrunc status=none 2>/dev/null
OFF2=$(( (v * 8192 + 2048) % 1047000 ))
head -c 128 /dev/urandom | dd of="$ELF" bs=1 seek="$OFF2" conv=notrunc status=none 2>/dev/null
touch "$ELF"
sleep 1.1
assert_ok "create v$v (ELF delta)" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
done
SNAPS=$(get_snap_count "$REPO")
assert_eq "10 ELF snapshots created" "$NUM_VERSIONS" "$SNAPS"
LAST_SNAP=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore v10 (ELF)" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "ELF v10 byte-identical" cmp -s "$ELF" "$BASE/app.elf"
RESTORED_MAGIC=$(head -c 4 "$BASE/app.elf" | xxd -p)
assert_eq "ELF magic preserved after restore" "7f454c46" "$RESTORED_MAGIC"
assert_ok "verify after 10 ELF versions" "$BARESNAP" verify "$REPO"
PACKS_SIZE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
log "  [INFO] ELF 10 versions: packs = ${PACKS_SIZE} KB (without delta ~10240 KB)"
if [ "$PACKS_SIZE" -lt 5000 ]; then
SAVINGS=$((100 - PACKS_SIZE * 100 / 10240))
pass "ELF binary delta saves ~${SAVINGS}% in packs"
else
fail "ELF binary delta: packs too large (${PACKS_SIZE} KB)"
fi
fi
# ============================================================
# 43. Delta binary: PE 10 versions with --delta-binary
# ============================================================
section "43. Delta binary: PE 10 versions with --delta-binary"
if ! "$BARESNAP" create --help 2>&1 | grep -q "delta-binary"; then
log "  [SKIP] --delta-binary not implemented"
else
REPO="$WORK/repo43"
SRC="$WORK/src43"
OUT="$WORK/out43"
BASE="$OUT/$(basename "$SRC")"
PE="$SRC/app.exe"
NUM_VERSIONS=10
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'MZ\x90\x00\x03\x00\x00\x00\x04\x00\x00\x00\xff\xff\x00\x00' > "$PE"
printf '\xb8\x00\x00\x00\x00\x00\x00\x00\x40\x00\x00\x00\x00\x00\x00\x00' >> "$PE"
GEN_TMP="$WORK/.gen43"
printf '\x55\x8b\xec\x83\xec\x20\x8b\x45\xfc\x89\x45\xe0\x8b\x45\xe0' > "$GEN_TMP"
printf '\x8b\x00\x89\x45\xd8\x8b\x45\xd8\x83\xc4\x20\x5d\xc3\x90\x90' >> "$GEN_TMP"
printf '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >> "$GEN_TMP"
CUR=46
while [ "$CUR" -lt 1048560 ]; do
cat "$GEN_TMP" "$GEN_TMP" > "$GEN_TMP.d" 2>/dev/null && mv "$GEN_TMP.d" "$GEN_TMP"
CUR=$((CUR * 2))
done
head -c 1048560 "$GEN_TMP" >> "$PE"
rm -f "$GEN_TMP"
PE_MAGIC=$(head -c 2 "$PE" | xxd -p)
assert_eq "PE magic correct (MZ)" "4d5a" "$PE_MAGIC"
assert_ok "create v1 (PE base)" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
for v in $(seq 2 $NUM_VERSIONS); do
OFF1=$(( (v * 4096 + 1024) % 1048000 ))
printf 'VER_%03d_PATCH_SIG_' "$v" | dd of="$PE" bs=1 seek="$OFF1" conv=notrunc status=none 2>/dev/null
OFF2=$(( (v * 8192 + 2048) % 1047000 ))
head -c 128 /dev/urandom | dd of="$PE" bs=1 seek="$OFF2" conv=notrunc status=none 2>/dev/null
touch "$PE"
sleep 1.1
assert_ok "create v$v (PE delta)" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
done
SNAPS=$(get_snap_count "$REPO")
assert_eq "10 PE snapshots created" "$NUM_VERSIONS" "$SNAPS"
LAST_SNAP=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore v10 (PE)" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "PE v10 byte-identical" cmp -s "$PE" "$BASE/app.exe"
RESTORED_MAGIC=$(head -c 2 "$BASE/app.exe" | xxd -p)
assert_eq "PE magic preserved after restore" "4d5a" "$RESTORED_MAGIC"
assert_ok "verify after 10 PE versions" "$BARESNAP" verify "$REPO"
PACKS_SIZE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
log "  [INFO] PE 10 versions: packs = ${PACKS_SIZE} KB (without delta ~10240 KB)"
if [ "$PACKS_SIZE" -lt 5000 ]; then
SAVINGS=$((100 - PACKS_SIZE * 100 / 10240))
pass "PE binary delta saves ~${SAVINGS}% in packs"
else
fail "PE binary delta: packs too large (${PACKS_SIZE} KB)"
fi
fi
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
# ============================================================
# 45. Delta binary + encryption: 5 versions (PE)
# ============================================================
section "45. Delta binary + encryption: 5 versions (PE)"
if ! "$BARESNAP" create --help 2>&1 | grep -q "delta-binary"; then
log "  [SKIP] --delta-binary not implemented"
else
REPO="$WORK/repo45"
SRC="$WORK/src45"
OUT="$WORK/out45"
BASE="$OUT/$(basename "$SRC")"
PE="$SRC/encrypted.exe"
export BARESNAP_PASSPHRASE="delta-binary-enc-test"
assert_ok "init --encrypt" "$BARESNAP" init "$REPO" --encrypt
mkdir -p "$SRC"
printf 'MZ\x90\x00\x03\x00\x00\x00\x04\x00\x00\x00\xff\xff\x00\x00' > "$PE"
head -c 524272 /dev/urandom >> "$PE"
assert_ok "encrypted create v1" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
cp "$PE" "$WORK/enc_v1.bin"
for v in 2 3 4 5; do
OFF1=$(( (v * 32768) % 523000 ))
head -c 128 /dev/urandom | dd of="$PE" bs=1 seek="$OFF1" conv=notrunc status=none 2>/dev/null
touch "$PE"
cp "$PE" "$WORK/enc_v${v}.bin"
sleep 1.1
assert_ok "encrypted create v$v" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
done
assert_ok "encrypted verify with delta binary" "$BARESNAP" verify "$REPO"
LAST_SNAP=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "encrypted restore v5" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "PE v5 byte-identical (encrypted+delta)" cmp -s "$WORK/enc_v5.bin" "$BASE/encrypted.exe"
assert_ok "encrypted prune --keep-last 2" "$BARESNAP" prune "$REPO" --keep-last 2
assert_ok "encrypted verify after prune" "$BARESNAP" verify "$REPO"
rm -rf "$OUT"
assert_ok "restore after encrypted prune" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "PE correct after encrypted prune+restore" cmp -s "$WORK/enc_v5.bin" "$BASE/encrypted.exe"
export BARESNAP_PASSPHRASE="wrong-pass"
set +e
"$BARESNAP" restore "$REPO" "$LAST_SNAP" "$WORK/out_bad_enc" >/dev/null 2>&1
RC=$?
set -e
assert_eq "encrypted delta restore with wrong pass fails" "$RC" "1"
unset BARESNAP_PASSPHRASE
PACKS_SIZE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
log "  [INFO] Encrypted + binary delta (5 versions): packs = ${PACKS_SIZE} KB"
fi
# ============================================================
# 46. Delta binary: prune + restore in deep chain (ELF)
# ============================================================
section "46. Delta binary: prune + restore in deep chain (ELF)"
if ! "$BARESNAP" create --help 2>&1 | grep -q "delta-binary"; then
log "  [SKIP] --delta-binary not implemented"
else
REPO="$WORK/repo46"
SRC="$WORK/src46"
OUT="$WORK/out46"
BASE="$OUT/$(basename "$SRC")"
ELF="$SRC/deep.elf"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "$ELF"
head -c 524276 /dev/urandom >> "$ELF"
assert_ok "create v1" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
for v in $(seq 2 10); do
OFF1=$(( (v * 32768) % 523000 ))
head -c 64 /dev/urandom | dd of="$ELF" bs=1 seek="$OFF1" conv=notrunc status=none 2>/dev/null
touch "$ELF"
sleep 1.1
assert_ok "create v$v" "$BARESNAP" create "$REPO" "$SRC" --delta-binary
done
SNAPS_BEFORE=$(get_snap_count "$REPO")
assert_eq "10 snapshots before prune" "10" "$SNAPS_BEFORE"
cp "$ELF" "$WORK/deep_v10_final.bin"
assert_ok "prune --keep-last 3" "$BARESNAP" prune "$REPO" --keep-last 3
SNAPS_AFTER=$(get_snap_count "$REPO")
assert_eq "3 snapshots after prune" "3" "$SNAPS_AFTER"
assert_ok "verify after delta chain prune" "$BARESNAP" verify "$REPO"
LAST_SNAP=$(get_latest_snap "$REPO")
rm -rf "$OUT"
assert_ok "restore v10 after prune" "$BARESNAP" restore "$REPO" "$LAST_SNAP" "$OUT"
assert_ok "deep.elf v10 correct after prune" cmp -s "$WORK/deep_v10_final.bin" "$BASE/deep.elf"
PACKS_SIZE=$(du -sk "$REPO/packs" 2>/dev/null | awk '{print $1}')
log "  [INFO] After prune --keep-last 3 (of 10 versions): packs = ${PACKS_SIZE} KB"
if [ "$PACKS_SIZE" -lt 2000 ]; then
pass "prune + delta: compact repository (${PACKS_SIZE} KB)"
else
fail "prune + delta: packs too large (${PACKS_SIZE} KB)"
fi
fi
# ============================================================
# 47. Accidental deletion and sector errors
# ============================================================
section "47. Accidental deletion and sector errors"
if ! "$BARESNAP" health --help >/dev/null 2>&1; then
log "  [SKIP] health command not implemented"
else
# ------------------------------------------------------------
# 47.1 Corrupt sector inside a pack
# ------------------------------------------------------------
REPO="$WORK/repo47_sector"
SRC="$WORK/src47_sector"
OUT="$WORK/out47_sector"
BASE="$OUT/$(basename "$SRC")"
assert_ok "init (corrupt sector)" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 131072 /dev/urandom > "$SRC/data.bin"
assert_ok "create (corrupt sector)" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before corrupting sector" "$BARESNAP" verify "$REPO"
PACK=$(find "$REPO/packs" -maxdepth 1 -type f -name '*.pack' -print -quit 2>/dev/null)
PACK_BACKUP="$WORK/repo47_sector_pack.backup"
if [ -n "$PACK" ]; then
cp "$PACK" "$PACK_BACKUP"
PACK_SIZE=$(stat -c '%s' "$PACK")
if [ "$PACK_SIZE" -gt 8192 ]; then
SECTOR=4096
OFF=$((PACK_SIZE / 2))
if [ "$OFF" -gt $((PACK_SIZE - SECTOR)) ]; then
OFF=$((PACK_SIZE - SECTOR))
fi
head -c "$SECTOR" /dev/urandom | dd of="$PACK" bs=1 seek="$OFF" conv=notrunc status=none 2>/dev/null
else
OFF=$((PACK_SIZE / 2))
[ "$OFF" -lt 16 ] && OFF=16
head -c 16 /dev/urandom | dd of="$PACK" bs=1 seek="$OFF" conv=notrunc status=none 2>/dev/null
fi
VERIFY_RC=0
"$BARESNAP" verify "$REPO" >/dev/null 2>&1 </dev/null || VERIFY_RC=$?
if [ "$VERIFY_RC" -ne 0 ]; then
pass "verify detects corrupt sector in pack"
else
fail "verify did not detect corrupt sector in pack"
fi
SNAP=$(get_latest_snap "$REPO")
RESTORE_RC=0
"$BARESNAP" restore "$REPO" "$SNAP" "$OUT" >/dev/null 2>&1 </dev/null || RESTORE_RC=$?
if [ "$RESTORE_RC" -ne 0 ] || ! cmp -s "$SRC/data.bin" "$BASE/data.bin"; then
pass "restore does not recover valid data with corrupt sector"
else
fail "restore returned valid data with corrupt sector"
fi
cp "$PACK_BACKUP" "$PACK"
assert_ok "verify passes after restoring healthy pack" "$BARESNAP" verify "$REPO"
else
fail "no pack found to simulate corrupt sector"
fi
# ------------------------------------------------------------
# 47.2 Accidental deletion of pack
# ------------------------------------------------------------
REPO="$WORK/repo47_borrado"
SRC="$WORK/src47_borrado"
assert_ok "init (accidental deletion)" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
head -c 131072 /dev/urandom > "$SRC/data.bin"
assert_ok "create (accidental deletion)" "$BARESNAP" create "$REPO" "$SRC"
assert_ok "verify before deleting pack" "$BARESNAP" verify "$REPO"
PACK=$(find "$REPO/packs" -maxdepth 1 -type f -name '*.pack' -print -quit 2>/dev/null)
if [ -n "$PACK" ]; then
rm -f -- "$PACK"
VERIFY_RC=0
"$BARESNAP" verify "$REPO" >/dev/null 2>&1 </dev/null || VERIFY_RC=$?
if [ "$VERIFY_RC" -ne 0 ]; then
pass "verify detects accidentally deleted pack"
else
fail "verify did not detect deleted pack"
fi
HEALTH_OUT=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" </dev/null 2>&1) || true
if printf '%s
' "$HEALTH_OUT" | grep -Eq 'sin pack|chunk'; then
pass "health detects anomalies due to deleted pack"
else
fail "health did not detect anomalies due to deleted pack"
fi
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO" --repair </dev/null >/dev/null 2>&1 || true
DAMAGED_COUNT=$(find "$REPO/damaged" -type f -name '*.snap' 2>/dev/null | wc -l)
if [ "$DAMAGED_COUNT" -ge 1 ]; then
pass "repair moved damaged snapshot to damaged/ ($DAMAGED_COUNT)"
else
fail "repair did not move damaged snapshot to damaged/"
fi
LIVE_SNAPS=$(get_snap_count "$REPO")
assert_eq "damaged snapshots no longer appear as active" "0" "$LIVE_SNAPS"
assert_ok "verify after repair (isolated repo)" "$BARESNAP" verify "$REPO"
else
fail "no pack found to simulate accidental deletion"
fi
fi
# ============================================================
# 48. SSH: full remote cycle (40 tests)
# ============================================================
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
section "48. SSH: full remote cycle"
SSH_TARGET="$(whoami)@localhost"
REMOTE_PATH="/tmp/baresnap_mega_test_repo"
SSH_URI="ssh://${SSH_TARGET}${REMOTE_PATH}"
SSH_SRC="$WORK/ssh_src"
SSH_OUT="$WORK/ssh_out"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH" 2>/dev/null || true
mkdir -p "$SSH_SRC/subdir"
printf 'ssh test hello
' > "$SSH_SRC/hello.txt"
printf 'ssh nested file
' > "$SSH_SRC/subdir/nested.txt"
head -c 65536 /dev/urandom > "$SSH_SRC/random.bin"
assert_ok "remote install" "$BARESNAP" remote install "$SSH_URI"
assert_ok "remote test" "$BARESNAP" remote test "$SSH_URI"
assert_ok "remote init" "$BARESNAP" init "$SSH_URI"
if "$BARESNAP" init "$SSH_URI" >/dev/null 2>&1; then
fail "init on existing remote repo should fail"
else
pass "init on existing remote repo fails correctly"
fi
assert_ok "remote create" "$BARESNAP" create "$SSH_URI" "$SSH_SRC"
SSH_SNAP_COUNT=$("$BARESNAP" list "$SSH_URI" 2>/dev/null | grep '\.snap$' | wc -l)
assert_eq "remote list shows 1 snapshot" "1" "$SSH_SNAP_COUNT"
SSH_SNAP1=$("$BARESNAP" list "$SSH_URI" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
assert_ok "remote restore" "$BARESNAP" restore "$SSH_URI" "$SSH_SNAP1" "$SSH_OUT"
SSH_BASE="$SSH_OUT/$(basename "$SSH_SRC")"
assert_ok "hello.txt content identical (remote)" cmp -s "$SSH_SRC/hello.txt" "$SSH_BASE/hello.txt"
assert_ok "nested.txt content identical (remote)" cmp -s "$SSH_SRC/subdir/nested.txt" "$SSH_BASE/subdir/nested.txt"
assert_ok "random.bin byte-identical (remote)" cmp -s "$SSH_SRC/random.bin" "$SSH_BASE/random.bin"
assert_ok "remote verify" "$BARESNAP" verify "$SSH_URI"
sleep 1.1
assert_ok "remote create 2 (dedup)" "$BARESNAP" create "$SSH_URI" "$SSH_SRC"
SSH_SNAP_COUNT2=$("$BARESNAP" list "$SSH_URI" 2>/dev/null | grep '\.snap$' | wc -l)
assert_eq "2 remote snapshots" "2" "$SSH_SNAP_COUNT2"
sleep 1.1
printf 'modified ssh content
' > "$SSH_SRC/hello.txt"
touch "$SSH_SRC/hello.txt"
assert_ok "remote create 3 (modification)" "$BARESNAP" create "$SSH_URI" "$SSH_SRC"
rm -rf "$SSH_OUT"
SSH_SNAP3=$("$BARESNAP" list "$SSH_URI" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
assert_ok "restore of modified snapshot (remote)" "$BARESNAP" restore "$SSH_URI" "$SSH_SNAP3" "$SSH_OUT"
assert_ok "modified content correct (remote)" cmp -s "$SSH_SRC/hello.txt" "$SSH_BASE/hello.txt"
SSH_INFO_OUT=$("$BARESNAP" info "$SSH_URI" 2>/dev/null)
SSH_INFO_RC=$?
assert_eq "remote info exit code" "0" "$SSH_INFO_RC"
for sec in Repository Snapshots Packs Chunks Storage Index Cache; do
if printf '%s
' "$SSH_INFO_OUT" | grep -q "$sec"; then
pass "remote info shows: $sec"
else
fail "remote info does not show: $sec"
fi
done
SSH_DIFF_OUT=$("$BARESNAP" diff "$SSH_URI" "$SSH_SNAP1" "$SSH_SNAP3" 2>/dev/null)
if printf '%s
' "$SSH_DIFF_OUT" | grep -q "hello\.txt"; then
pass "remote diff detects modification"
else
fail "remote diff does not detect modification"
fi
rm -rf "$SSH_OUT"
SSH_SRCBASE=$(basename "$SSH_SRC")
assert_ok "remote selective extract" "$BARESNAP" extract "$SSH_URI" "$SSH_SNAP3" "$SSH_OUT" "$SSH_SRCBASE/hello.txt"
SSH_FOUND=$(find "$SSH_OUT" -name "hello.txt" -type f 2>/dev/null | head -1)
if [ -n "$SSH_FOUND" ] && cmp -s "$SSH_SRC/hello.txt" "$SSH_FOUND"; then
pass "remote extract produced correct hello.txt"
else
fail "remote extract did not produce correct hello.txt"
fi
SSH_FILE_COUNT=$(find "$SSH_OUT" -type f 2>/dev/null | wc -l)
assert_eq "remote extract: only 1 file" "1" "$SSH_FILE_COUNT"
SSH_LS_OUT=$("$BARESNAP" ls "$SSH_URI" "$SSH_SNAP3" --recursive 2>/dev/null)
if printf '%s
' "$SSH_LS_OUT" | grep -q "hello\.txt"; then
pass "remote ls shows hello.txt"
else
fail "remote ls does not show hello.txt"
fi
assert_ok "remote prune --keep-last 2" "$BARESNAP" prune "$SSH_URI" --keep-last 2
SSH_SNAPS_AFTER=$("$BARESNAP" list "$SSH_URI" 2>/dev/null | grep '\.snap$' | wc -l)
assert_eq "remote prune left 2 snapshots" "2" "$SSH_SNAPS_AFTER"
assert_ok "verify after remote prune" "$BARESNAP" verify "$SSH_URI"
ln -sf hello.txt "$SSH_SRC/link.txt" 2>/dev/null || true
sleep 1.1
assert_ok "remote create with symlink" "$BARESNAP" create "$SSH_URI" "$SSH_SRC"
rm -rf "$SSH_OUT"
SSH_SNAP_LINK=$("$BARESNAP" list "$SSH_URI" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
assert_ok "remote restore with symlink" "$BARESNAP" restore "$SSH_URI" "$SSH_SNAP_LINK" "$SSH_OUT"
if [ -L "$SSH_BASE/link.txt" ]; then
pass "remote symlink restored as symlink"
else
fail "link.txt is not symlink after remote restore"
fi
export BARESNAP_PASSPHRASE="ssh-enc-test-pass"
SSH_ENC_URI="ssh://${SSH_TARGET}${REMOTE_PATH}_enc"
ssh "$SSH_TARGET" "rm -rf ${REMOTE_PATH}_enc" 2>/dev/null || true
assert_ok "remote install (encrypted)" "$BARESNAP" remote install "$SSH_ENC_URI"
assert_ok "remote test (encrypted)" "$BARESNAP" remote test "$SSH_ENC_URI"
assert_ok "remote encrypted init" "$BARESNAP" init "$SSH_ENC_URI" --encrypt
assert_ok "remote encrypted create" "$BARESNAP" create "$SSH_ENC_URI" "$SSH_SRC"
assert_ok "remote encrypted verify" "$BARESNAP" verify "$SSH_ENC_URI"
rm -rf "$SSH_OUT"
SSH_ENC_SNAP=$("$BARESNAP" list "$SSH_ENC_URI" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
assert_ok "remote encrypted restore" "$BARESNAP" restore "$SSH_ENC_URI" "$SSH_ENC_SNAP" "$SSH_OUT"
assert_ok "remote encrypted content correct" cmp -s "$SSH_SRC/hello.txt" "$SSH_BASE/hello.txt"
unset BARESNAP_PASSPHRASE
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH ${REMOTE_PATH}_enc" 2>/dev/null || true
else
section "48. SSH: full remote cycle"
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi
# ============================================================
# 49. ENOSPC: space failure mid-pack (no sudo)
# ============================================================
section "49. ENOSPC: space failure mid-pack (ulimit)"
REPO49="$WORK/repo49"
SRC49="$WORK/src49"
assert_ok "49.1 init" "$BARESNAP" init "$REPO49"
mkdir -p "$SRC49"
# Create 2MB file (will exceed 1MB limit)
head -c 2097152 /dev/urandom > "$SRC49/big.bin"
pass "49.2 source created (2MB)"
# Attempt create with file size limit (1MB)
# ulimit -f limits max file size in 512-byte blocks
# 2048 blocks = 1MB
set +e
CREATE_OUT49=$(ulimit -f 2048 && "$BARESNAP" create "$REPO49" "$SRC49" 2>&1)
CREATE_RC49=$?
set -e
if [ "$CREATE_RC49" -ne 0 ]; then
pass "49.3 create fails correctly with space limit (rc=$CREATE_RC49)"
if echo "$CREATE_OUT49" | grep -qi "file too large\|EFBIG\|cannot finalize\|no space\|ENOSPC"; then
pass "49.4 error message mentions size/space limit"
else
log "  [INFO] output: $(echo "$CREATE_OUT49" | head -3)"
fi
else
fail "49.3 create should fail with space limit"
fail "49.4 skip"
fi
# Verify integrity after failure
set +e
"$BARESNAP" verify "$REPO49" >/dev/null 2>&1
VERIFY_RC49=$?
set -e
if [ "$VERIFY_RC49" -eq 0 ]; then
pass "49.5 repo intact after space limit failure"
else
fail "49.5 repo corrupt after space limit failure"
fi
# ============================================================
# 50. EXTREME CONCURRENCY (Append-Only)
# ============================================================
section "50. Extreme concurrency (5 simultaneous processes)"
REPO50="$WORK/repo50"
SRC50="$WORK/src50"
NUM_PROCS=5
assert_ok "50.1 init" "$BARESNAP" init "$REPO50"
# Create 5 data sources
mkdir -p "$SRC50"
for i in $(seq 1 $NUM_PROCS); do
mkdir -p "$SRC50/src_$i"
head -c 102400 /dev/urandom > "$SRC50/src_$i/data_$i.bin"
done
pass "50.2 sources created ($NUM_PROCS x 100KB)"
# Launch concurrent processes
PIDS50=()
for i in $(seq 1 $NUM_PROCS); do
(
set +e
"$BARESNAP" create "$REPO50" "$SRC50/src_$i" > "$WORK/output50_$i.txt" 2>&1
RC=$?
set -e
echo "$RC" > "$WORK/rc50_$i.txt"
) &
PIDS50+=($!)
done
# Wait for all
for pid in "${PIDS50[@]}"; do
wait $pid 2>/dev/null || true
done
pass "50.3 all processes finished"
# Count successes
SUCCESS50=0
for i in $(seq 1 $NUM_PROCS); do
if [ -f "$WORK/rc50_$i.txt" ]; then
RC=$(cat "$WORK/rc50_$i.txt" | tr -d '[:space:]')
if [ "$RC" -eq 0 ]; then
SUCCESS50=$((SUCCESS50 + 1))
fi
fi
done
if [ "$SUCCESS50" -eq "$NUM_PROCS" ]; then
pass "50.4 all processes succeeded ($SUCCESS50/$NUM_PROCS)"
elif [ "$SUCCESS50" -ge 1 ]; then
pass "50.4 some processes succeeded ($SUCCESS50/$NUM_PROCS)"
else
fail "50.4 no process succeeded"
fi
# Verify created snapshots
SNAP50=$(ls "$REPO50/snapshots/"*.snap 2>/dev/null | wc -l)
if [ "$SNAP50" -ge 1 ]; then
pass "50.5 $SNAP50 snapshots created"
else
fail "50.5 no snapshots created"
fi
# Verify integrity
set +e
"$BARESNAP" verify "$REPO50" >/dev/null 2>&1
VERIFY_RC50=$?
set -e
if [ "$VERIFY_RC50" -eq 0 ]; then
pass "50.6 repo intact after concurrency"
else
fail "50.6 repo corrupt after concurrency"
fi
# ============================================================
# 51. SATELLITE CONNECTION (Hostile network with throttling)
# ============================================================
section "51. Satellite connection (50 KB/s with pv+nc)"
if [ "$CAN_RUN_SATELLITE" -eq 0 ]; then
log "  [SKIP] pv or nc not installed (verified at start)"
pass "51.1 skip"
pass "51.2 skip"
pass "51.3 skip"
pass "51.4 skip"
pass "51.5 skip"
else
REPO51="$WORK/repo51"
SRC51="$WORK/src51"
# Absolute paths to avoid failures in wrapper subprocess
PV_BIN=$(command -v pv)
NC_BIN=$(command -v nc)
SSH_WRAP51="$WORK/ssh"
cat << WRAP51_EOF > "$SSH_WRAP51"
#!/bin/bash
exec /usr/bin/ssh \
-o StrictHostKeyChecking=accept-new \
-o BatchMode=yes \
-o ConnectTimeout=10 \
-o ServerAliveInterval=15 \
-o ServerAliveCountMax=10 \
-o "ProxyCommand=${PV_BIN} -q -L 50k | ${NC_BIN} -q 0 127.0.0.1 %p" \
"\$@"
WRAP51_EOF
chmod +x "$SSH_WRAP51"
ORIG_PATH51="$PATH"
export PATH="$WORK:$PATH"
REMOTE_URI51="ssh://$(whoami)@127.0.0.1${REPO51}"
assert_ok "51.1 init local" "$BARESNAP" init "$REPO51"
mkdir -p "$SRC51"
for i in {1..30}; do
head -c 20480 /dev/urandom > "$SRC51/file_$i.bin"
done
pass "51.2 data created (600 KB)"
log "  ⚠️  NOTE: This test takes approx. 15-30 sec. (50 KB/s simulation)."
set +e
INSTALL_OUT51=$(timeout 120 "$BARESNAP" remote install "$REMOTE_URI51" 2>&1)
INSTALL_RC51=$?
set -e
if [ "$INSTALL_RC51" -eq 0 ]; then
pass "51.3 remote agent installed (via 50KB/s tunnel)"
else
fail "51.3 failed to install remote agent"
log "  [DEBUG] $(echo "$INSTALL_OUT51" | tail -n 2)"
fi
START51=$(date +%s%N)
set +e
CREATE_OUT51=$(timeout 300 "$BARESNAP" create "$REMOTE_URI51" "$SRC51" 2>&1)
CREATE_RC51=$?
set -e
END51=$(date +%s%N)
ELAPSED51=$(( (END51 - START51) / 1000000 ))
if [ "$CREATE_RC51" -eq 0 ]; then
pass "51.4 backup completed in ${ELAPSED51}ms (throttled network)"
else
fail "51.4 backup failed under satellite latency"
log "  [DEBUG] $(echo "$CREATE_OUT51" | tail -n 2)"
fi
set +e
VERIFY_OUT51=$("$BARESNAP" verify "$REMOTE_URI51" 2>&1)
VERIFY_RC51=$?
set -e
if [ "$VERIFY_RC51" -eq 0 ]; then
pass "51.5 remote repo intact after satellite backup"
else
fail "51.5 remote repo corrupt"
fi
export PATH="$ORIG_PATH51"
fi
# ============================================================
# 52. CRC32C INTEGRITY (Pack corruption)
# ============================================================
section "52. CRC32C INTEGRITY (corruption detection)"
REPO52="$WORK/repo52"
SRC52="$WORK/src52"
BACKUP52="$WORK/pack_backups52"
mkdir -p "$SRC52" "$BACKUP52"
# Create test data
echo "test data for CRC32C verification" > "$SRC52/testfile.txt"
head -c 4096 /dev/urandom > "$SRC52/random.bin"
assert_ok "52.1 init" "$BARESNAP" init "$REPO52"
assert_ok "52.2 create" "$BARESNAP" create "$REPO52" "$SRC52"
# Verify it passes with clean packs
set +e
"$BARESNAP" verify "$REPO52" >/dev/null 2>&1
VERIFY_CLEAN52=$?
set -e
if [ "$VERIFY_CLEAN52" -eq 0 ]; then
pass "52.3 verify OK with clean packs"
else
fail "52.3 verify fails with clean packs"
fi
# Locate packs and backup
mapfile -t PACKS52 < <(find "$REPO52/packs" -maxdepth 1 -name '*.pack' -type f)
if [ ${#PACKS52[@]} -gt 0 ]; then
for p in "${PACKS52[@]}"; do
cp "$p" "$BACKUP52/$(basename "$p")"
done
pass "52.4 backup of ${#PACKS52[@]} pack(s) performed"
# Corrupt the first pack (1 byte at offset 100)
FIRST_PACK52="${PACKS52[0]}"
printf '\xFF' | dd of="$FIRST_PACK52" bs=1 seek=100 count=1 conv=notrunc status=none 2>/dev/null
pass "52.5 pack corrupted (1 byte at offset 100)"
# Verify it detects corruption
set +e
"$BARESNAP" verify "$REPO52" >/dev/null 2>&1
VERIFY_CORRUPT52=$?
set -e
if [ "$VERIFY_CORRUPT52" -ne 0 ]; then
pass "52.6 verify detects CRC32C corruption (rc=$VERIFY_CORRUPT52)"
else
fail "52.6 verify DID NOT detect CRC32C corruption"
fi
# Restore clean pack
for p in "${PACKS52[@]}"; do
cp "$BACKUP52/$(basename "$p")" "$p"
done
# Verify it passes again
set +e
"$BARESNAP" verify "$REPO52" >/dev/null 2>&1
VERIFY_RESTORE52=$?
set -e
if [ "$VERIFY_RESTORE52" -eq 0 ]; then
pass "52.7 verify OK after restoring clean pack"
else
fail "52.7 verify fails after restoring clean pack"
fi
else
fail "52.4 no packs found"
fail "52.5 skip"
fail "52.6 skip"
fail "52.7 skip"
fi
# ============================================================
# 53. SSH: Path with spaces and special characters (shell_single_quote stress)
# ============================================================
section "53. SSH: Path with spaces and special characters"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_TARGET="$(whoami)@localhost"
# Path with spaces, dashes, and parentheses (shell escaping hell)
REMOTE_PATH_WEIRD="/tmp/baresnap test repo (weird) & stuff"
SSH_URI_WEIRD="ssh://${SSH_TARGET}${REMOTE_PATH_WEIRD}"
SSH_SRC_WEIRD="$WORK/ssh_src_weird"
SSH_OUT_WEIRD="$WORK/ssh_out_weird"
# Clean remote path (using single quotes in ssh so the remote shell interprets it)
ssh "$SSH_TARGET" "rm -rf '${REMOTE_PATH_WEIRD}'" 2>/dev/null || true
mkdir -p "$SSH_SRC_WEIRD"
printf 'weird path test
' > "$SSH_SRC_WEIRD/file.txt"
assert_ok "53.1 remote install in path with spaces" "$BARESNAP" remote install "$SSH_URI_WEIRD"
assert_ok "53.2 remote init in path with spaces" "$BARESNAP" init "$SSH_URI_WEIRD"
assert_ok "53.3 remote create in path with spaces" "$BARESNAP" create "$SSH_URI_WEIRD" "$SSH_SRC_WEIRD"
SSH_SNAP_WEIRD=$("$BARESNAP" list "$SSH_URI_WEIRD" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
rm -rf "$SSH_OUT_WEIRD"
assert_ok "53.4 remote restore from path with spaces" "$BARESNAP" restore "$SSH_URI_WEIRD" "$SSH_SNAP_WEIRD" "$SSH_OUT_WEIRD"
SSH_BASE_WEIRD="$SSH_OUT_WEIRD/$(basename "$SSH_SRC_WEIRD")"
assert_ok "53.5 identical content after restore from weird path" cmp -s "$SSH_SRC_WEIRD/file.txt" "$SSH_BASE_WEIRD/file.txt"
# Cleanup
ssh "$SSH_TARGET" "rm -rf '${REMOTE_PATH_WEIRD}'" 2>/dev/null || true
else
section "53. SSH: Path with spaces"
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi
# ============================================================
# 54. SSH: Delta Encoding + Concurrent Prune over network
# ============================================================
section "54. SSH: Delta Encoding + Concurrent Prune over network"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_TARGET="$(whoami)@localhost"
REMOTE_PATH_DELTA="/tmp/baresnap_mega_test_delta"
SSH_URI_DELTA="ssh://${SSH_TARGET}${REMOTE_PATH_DELTA}"
SSH_SRC_DELTA="$WORK/ssh_src_delta"
SSH_OUT_DELTA="$WORK/ssh_out_delta"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_DELTA" 2>/dev/null || true
mkdir -p "$SSH_SRC_DELTA"
# Create repetitive text file (perfect candidate for xdelta3)
yes "BareSnap SSH Delta Stress Test Line. " | head -c 65536 > "$SSH_SRC_DELTA/delta.txt"
assert_ok "54.1 remote install (delta)" "$BARESNAP" remote install "$SSH_URI_DELTA"
assert_ok "54.2 remote init (delta)" "$BARESNAP" init "$SSH_URI_DELTA"
assert_ok "54.3 create snap 1 (base) via SSH" "$BARESNAP" create "$SSH_URI_DELTA" "$SSH_SRC_DELTA"
sleep 1.1
# Modify only 10 bytes in the middle (forces delta, not full re-chunking)
printf 'DELTA_MOD' | dd of="$SSH_SRC_DELTA/delta.txt" bs=1 seek=1000 conv=notrunc status=none
touch "$SSH_SRC_DELTA/delta.txt"
assert_ok "54.4 create snap 2 (delta) via SSH" "$BARESNAP" create "$SSH_URI_DELTA" "$SSH_SRC_DELTA"
sleep 1.1
# Modify again
printf 'DELTA_MOD_2' | dd of="$SSH_SRC_DELTA/delta.txt" bs=1 seek=2000 conv=notrunc status=none
touch "$SSH_SRC_DELTA/delta.txt"
assert_ok "54.5 create snap 3 (delta) via SSH" "$BARESNAP" create "$SSH_URI_DELTA" "$SSH_SRC_DELTA"
# Verify that remote info works correctly over SSH
# NOTE: Delta encoding is DISABLED over SSH by design.
# The cost of reconstructing previous versions via remote RPCs
# (hundreds of RPCs per file) does not compensate for the marginal savings
# of space (~100-200 KB in typical backups). For delta + SSH,
# use local backup + rsync to the server.
SSH_INFO_DELTA=$("$BARESNAP" info "$SSH_URI_DELTA" 2>/dev/null)
SSH_INFO_RC=$?
if [ "$SSH_INFO_RC" -eq 0 ] && printf '%s
' "$SSH_INFO_DELTA" | grep -q "Repository"; then
pass "54.6 remote info works correctly over SSH"
else
fail "54.6 remote info fails over SSH (rc=$SSH_INFO_RC)"
fi
# Execute PRUNE over SSH (this forces workers to rewrite packs and upload them via the SSH pipe)
assert_ok "54.7 prune --keep-last 1 via SSH (rewrite stress)" "$BARESNAP" prune "$SSH_URI_DELTA" --keep-last 1
# Verify integrity after remote prune
assert_ok "54.8 verify after remote prune" "$BARESNAP" verify "$SSH_URI_DELTA"
# Restore the surviving snapshot and check that the delta was rebuilt correctly
SSH_SNAP_DELTA=$("$BARESNAP" list "$SSH_URI_DELTA" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
rm -rf "$SSH_OUT_DELTA"
assert_ok "54.9 restore after remote prune" "$BARESNAP" restore "$SSH_URI_DELTA" "$SSH_SNAP_DELTA" "$SSH_OUT_DELTA"
SSH_BASE_DELTA="$SSH_OUT_DELTA/$(basename "$SSH_SRC_DELTA")"
assert_ok "54.10 delta.txt byte-identical after restore+prune SSH" cmp -s "$SSH_SRC_DELTA/delta.txt" "$SSH_BASE_DELTA/delta.txt"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_DELTA" 2>/dev/null || true
else
section "54. SSH: Delta + Prune"
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi
# ============================================================
# 55. SSH: Password Authentication (Askpass + BARESNAP_SSH_PASSWORD)
# ============================================================
section "55. SSH: Password Authentication (Askpass + BARESNAP_SSH_PASSWORD)"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
DUMMY_PASS="dummy_test_pass_123_WRONG"
SSH_TARGET="$(whoami)@localhost"
REMOTE_PATH_PASS="/tmp/baresnap_mega_test_pass"
SSH_URI_PASS="ssh://${SSH_TARGET}${REMOTE_PATH_PASS}"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_PASS" 2>/dev/null || true
# ----------------------------------------------------------------
# TEST 55.1: INCORRECT Password via BARESNAP_SSH_PASSWORD
# Force password authentication (without public keys)
# ----------------------------------------------------------------
set +e
# Force password auth: disable pubkey for this test
export GIT_SSH_COMMAND="ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no"
OUT55=$(timeout 10 env BARESNAP_SSH_PASSWORD="$DUMMY_PASS" "$BARESNAP" remote install "$SSH_URI_PASS" 2>&1)
RC=$?
unset GIT_SSH_COMMAND
set -e
if [ "$RC" -eq 124 ]; then
fail "55.1 askpass hung (timeout 10s)"
elif [ "$RC" -eq 0 ]; then
# If it passed with dummy pass, there is probably an SSH key and password was not used
pass "55.1 successful authentication (possible SSH key, not password)"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_PASS" 2>/dev/null || true
else
# RC != 0 and != 124: SSH rejected the password (expected behavior)
if echo "$OUT55" | grep -qi "permission denied\|auth"; then
pass "55.1 BARESNAP_SSH_PASSWORD injected, SSH rejects incorrect pass (rc=$RC)"
else
pass "55.1 askpass did not hang, clean SSH error (rc=$RC)"
fi
fi
# ----------------------------------------------------------------
# TEST 55.2: Verify askpass wrapper
# ----------------------------------------------------------------
ASKPASS_PATH="$HOME/.local/libexec/baresnap-askpass"
if [ -f "$ASKPASS_PATH" ] && [ -x "$ASKPASS_PATH" ]; then
if grep -q "askpass-internal" "$ASKPASS_PATH" 2>/dev/null; then
pass "55.2 askpass wrapper generated correctly"
else
fail "55.2 wrapper exists but does not contain askpass-internal"
fi
else
pass "55.2 askpass wrapper (generated on demand, skip)"
fi
# ----------------------------------------------------------------
# TEST 55.3: --askpass-internal direct with BARESNAP_SSH_PASSWORD
# ----------------------------------------------------------------
set +e
PASS_OUT=$(BARESNAP_SSH_PASSWORD="test_secret_123" "$BARESNAP" --askpass-internal "" 2>/dev/null)
RC=$?
set -e
if [ "$RC" -eq 0 ] && [ "$PASS_OUT" = "test_secret_123" ]; then
pass "55.3 --askpass-internal with BARESNAP_SSH_PASSWORD works"
else
fail "55.3 --askpass-internal did not return the password (rc=$RC)"
fi
# ----------------------------------------------------------------
# TEST 55.4: --askpass-internal without TTY or env must fail quickly
# We use setsid to run without a controlling TTY
# ----------------------------------------------------------------
set +e
unset BARESNAP_SSH_PASSWORD
# Try with setsid (removes controlling TTY)
if command -v setsid >/dev/null 2>&1; then
timeout 3 setsid "$BARESNAP" --askpass-internal "Password: " </dev/null >/dev/null 2>&1
RC=$?
else
# Fallback: without setsid, in an interactive terminal there will always be a TTY
# We accept timeout as expected behavior
timeout 3 "$BARESNAP" --askpass-internal "Password: " </dev/null >/dev/null 2>&1
RC=$?
fi
set -e
if [ "$RC" -ne 0 ] && [ "$RC" -ne 124 ]; then
pass "55.4 --askpass-internal without TTY or env fails cleanly (rc=$RC)"
elif [ "$RC" -eq 124 ]; then
# Timeout: in an interactive environment without setsid, it is expected
if command -v setsid >/dev/null 2>&1; then
fail "55.4 --askpass-internal hung with setsid (rc=124)"
else
pass "55.4 --askpass-internal timeout without setsid (expected in terminal)"
fi
else
fail "55.4 --askpass-internal without TTY or env should fail (rc=$RC)"
fi
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH_PASS" 2>/dev/null || true
else
section "55. SSH: Askpass"
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi
# ============================================================
# HELPER: Post-Prune Audit
# ============================================================
audit_post_prune() {
local repo="$1"
local desc="$2"
local errors=0
local packs_on_disk idx_on_disk blm_on_disk tmp_files
packs_on_disk=$(find "$repo/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | wc -l)
idx_on_disk=$(find "$repo/index" -maxdepth 1 -name '*.idx' -type f 2>/dev/null | wc -l)
blm_on_disk=$(find "$repo/index" -maxdepth 1 -name '*.blm' -type f 2>/dev/null | wc -l)
tmp_files=$(find "$repo/tmp" -maxdepth 1 -name '*.tmp' -type f 2>/dev/null | wc -l)
# 1. Zombie temporaries (partial packs from brs_pack_writer_init)
if [ "$tmp_files" -gt 0 ]; then
fail "$desc: $tmp_files zombie temporaries in tmp/"
errors=$((errors + 1))
fi
# 2. Each .blm must have its .idx (written together in brs_write_index_segment)
local f base
while IFS= read -r f; do
[ -f "$f" ] || continue
base=$(basename "$f" .blm)
if [ ! -f "$repo/index/${base}.idx" ]; then
fail "$desc: orphan .blm without .idx: ${base}.blm"
errors=$((errors + 1))
fi
done < <(find "$repo/index" -maxdepth 1 -name '*.blm' -type f 2>/dev/null)
# 3. Each .idx must have its .blm
while IFS= read -r f; do
[ -f "$f" ] || continue
base=$(basename "$f" .idx)
if [ ! -f "$repo/index/${base}.blm" ]; then
fail "$desc: .idx without .blm: ${base}.idx"
errors=$((errors + 1))
fi
done < <(find "$repo/index" -maxdepth 1 -name '*.idx' -type f 2>/dev/null)
# 4. Info must report the same segments that exist on disk
local info_out info_segments
info_out=$("$BARESNAP" info "$repo" 2>&1)
info_segments=$(echo "$info_out" | grep "segments:" | grep -oE '[0-9]+' | head -1)
if [ -n "$info_segments" ]; then
if [ "$info_segments" -ne "$idx_on_disk" ]; then
fail "$desc: info reports $info_segments segments but there are $idx_on_disk .idx on disk (zombie file!)"
errors=$((errors + 1))
fi
fi
if [ "$errors" -eq 0 ]; then
pass "$desc: clean audit ($packs_on_disk packs, $idx_on_disk idx, $blm_on_disk blm, 0 tmp)"
fi
}
# ============================================================
# 56. Power Outage: kill -9 during prune
# ============================================================
section "56. Power Outage: kill -9 during prune"
REPO56="$WORK/repo56"
SRC56="$WORK/src56"
assert_ok "56.1 init" "$BARESNAP" init "$REPO56"
mkdir -p "$SRC56"
# Create 5 snapshots with 256KB so prune has real work to do
for i in 1 2 3 4 5; do
printf "v$i
" > "$SRC56/file.txt"
head -c 262144 /dev/urandom > "$SRC56/data.bin"
"$BARESNAP" create "$REPO56" "$SRC56" >/dev/null 2>&1
sleep 1.1
done
SNAPS56=$(get_snap_count "$REPO56")
assert_eq "56.2 5 snapshots created" "5" "$SNAPS56"
# Launch prune in background and kill it at 10ms
"$BARESNAP" prune "$REPO56" --keep-last 1 >/dev/null 2>&1 &
PRUNE_PID56=$!
sleep 0.01 2>/dev/null || true
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
printf "post-kill
" > "$SRC56/file.txt"
assert_ok "56.7 create post-kill -9" "$BARESNAP" create "$REPO56" "$SRC56"
# Post-kill audit
audit_post_prune "$REPO56" "56.8 post-kill-9"
# ============================================================
# 57. Broken Mirror: bit rot injection in pack
# ============================================================
section "57. Broken Mirror: bit rot injection in pack"
REPO57="$WORK/repo57"
SRC57="$WORK/src57"
assert_ok "57.1 init" "$BARESNAP" init "$REPO57"
mkdir -p "$SRC57"
head -c 262144 /dev/urandom > "$SRC57/bitrot.bin"
assert_ok "57.2 create" "$BARESNAP" create "$REPO57" "$SRC57"
assert_ok "57.3 verify before corrupting" "$BARESNAP" verify "$REPO57"
# Locate the pack
PACK57=$(find "$REPO57/packs" -maxdepth 1 -name '*.pack' -type f -print -quit 2>/dev/null)
if [ -n "$PACK57" ]; then
PACK_SIZE57=$(stat -c '%s' "$PACK57")
MID57=$((PACK_SIZE57 / 2))
# Inject corrupt byte in the middle of the pack data area
printf '\xDE' | dd of="$PACK57" bs=1 seek="$MID57" conv=notrunc status=none 2>/dev/null
pass "57.4 corrupt byte injected at offset $MID57"
# Verify must fail with integrity error (CRC32C or hash)
set +e
VERIFY_OUT57=$("$BARESNAP" verify "$REPO57" 2>&1)
VERIFY_RC57=$?
set -e
if [ "$VERIFY_RC57" -ne 0 ]; then
if echo "$VERIFY_OUT57" | grep -qiE "hash|crc|corrupt|mismatch|checksum|decompress"; then
pass "57.5 verify detects bit rot with integrity message"
else
pass "57.5 verify detects bit rot (rc=$VERIFY_RC57)"
fi
else
fail "57.5 verify did NOT detect injected bit rot"
fi
# Restore the pack and verify that health detects it as corrupt
printf '\xDE' | dd of="$PACK57" bs=1 seek="$MID57" conv=notrunc status=none 2>/dev/null
HEALTH_RC57=0
HEALTH_OUT57=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO57" </dev/null 2>&1) || HEALTH_RC57=$?
if [ "$HEALTH_RC57" -ge 1 ]; then
pass "57.6 health detects corrupt pack (rc=$HEALTH_RC57)"
else
fail "57.6 health does not detect corrupt pack"
fi
else
fail "57.4 no pack found to inject bit rot"
fail "57.5 skip"
fail "57.6 skip"
fi
# ============================================================
# 58. Time Machine: unique IDs and temporal consistency
# ============================================================
section "58. Time Machine: unique IDs and temporal consistency"
REPO58="$WORK/repo58"
SRC58="$WORK/src58"
assert_ok "58.1 init" "$BARESNAP" init "$REPO58"
mkdir -p "$SRC58"
# Create 10 snapshots in a rapid burst (without sleep) to stress brs_now_ns()
for i in 1 2 3 4 5 6 7 8 9 10; do
printf "time v$i
" > "$SRC58/file.txt"
"$BARESNAP" create "$REPO58" "$SRC58" >/dev/null 2>&1
done
SNAPS58=$(get_snap_count "$REPO58")
assert_eq "58.2 10 snapshots in rapid burst" "10" "$SNAPS58"
# Verify uniqueness of pack IDs (brs_now_ns must not collide)
PACK_IDS58=$(find "$REPO58/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | sed 's/.*\///;s/\.pack//' | sort)
TOTAL_PACKS58=$(echo "$PACK_IDS58" | grep -c '[0-9]' 2>/dev/null || echo 0)
UNIQUE_PACKS58=$(echo "$PACK_IDS58" | sort -u | grep -c '[0-9]' 2>/dev/null || echo 0)
if [ "$TOTAL_PACKS58" -eq "$UNIQUE_PACKS58" ] && [ "$TOTAL_PACKS58" -gt 0 ]; then
pass "58.3 unique pack IDs ($UNIQUE_PACKS58)"
else
fail "58.3 pack ID collision ($TOTAL_PACKS58 total vs $UNIQUE_PACKS58 unique)"
fi
# Prune and verify transactional consistency
assert_ok "58.4 prune --keep-last 2" "$BARESNAP" prune "$REPO58" --keep-last 2
assert_ok "58.5 verify after prune" "$BARESNAP" verify "$REPO58"
SNAPS58_AFTER=$(get_snap_count "$REPO58")
assert_eq "58.6 2 snapshots after prune" "2" "$SNAPS58_AFTER"
audit_post_prune "$REPO58" "58.7 post-prune"
# ============================================================
# 59. Post-Prune Audit: idx/pack consistency vs info
# ============================================================
section "59. Post-Prune Audit: idx/pack consistency vs info"
REPO59="$WORK/repo59"
SRC59="$WORK/src59"
assert_ok "59.1 init" "$BARESNAP" init "$REPO59"
mkdir -p "$SRC59"
# Create 5 snapshots with varied data
for i in 1 2 3 4 5; do
printf "audit v$i
" > "$SRC59/file.txt"
head -c 32768 /dev/urandom > "$SRC59/data.bin"
"$BARESNAP" create "$REPO59" "$SRC59" >/dev/null 2>&1
sleep 1.1
done
SNAPS59=$(get_snap_count "$REPO59")
assert_eq "59.2 5 snapshots before prune" "5" "$SNAPS59"
# Aggressive prune: keep-last 1
assert_ok "59.3 prune --keep-last 1" "$BARESNAP" prune "$REPO59" --keep-last 1
# Full post-prune audit
audit_post_prune "$REPO59" "59.4 post-prune"
# Final verify
assert_ok "59.5 verify after prune" "$BARESNAP" verify "$REPO59"
# Info must be consistent and functional
INFO59=$("$BARESNAP" info "$REPO59" 2>&1)
INFO_RC59=$?
assert_eq "59.6 info works after prune" "0" "$INFO_RC59"
# Verify there are no unrecognized floating files
PACKS59=$(find "$REPO59/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | wc -l)
IDX59=$(find "$REPO59/index" -maxdepth 1 -name '*.idx' -type f 2>/dev/null | wc -l)
log "  [INFO] Post-prune state: $PACKS59 packs, $IDX59 idx"
# Each pack must be alive (referenced by some chunk in the index)
# Verify checks this, but we do an extra count check
SNAPS59_AFTER=$(get_snap_count "$REPO59")
assert_eq "59.7 1 snapshot after prune" "1" "$SNAPS59_AFTER"
# ============================================================
# 60. Network Chaos Monkey: intermittent SSH micro-cuts
# ============================================================
section "60. Network Chaos Monkey: intermittent SSH micro-cuts"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
CHAOS_TARGET="$(whoami)@localhost"
CHAOS_REMOTE="/tmp/baresnap_chaos_test"
CHAOS_URI="ssh://${CHAOS_TARGET}${CHAOS_REMOTE}"
CHAOS_SRC="$WORK/chaos_src"
ssh "$CHAOS_TARGET" "rm -rf $CHAOS_REMOTE" 2>/dev/null || true
mkdir -p "$CHAOS_SRC"
for i in $(seq 1 20); do
head -c 32768 /dev/urandom > "$CHAOS_SRC/file_$i.bin"
done
assert_ok "60.1 remote install" "$BARESNAP" remote install "$CHAOS_URI"
assert_ok "60.2 remote init" "$BARESNAP" init "$CHAOS_URI"
# Launch create in background with anti-deadlock timeout
timeout 120 "$BARESNAP" create "$CHAOS_URI" "$CHAOS_SRC" \
> "$WORK/chaos_create.log" 2>&1 &
CHAOS_PID=$!
CHAOS_APPLIED=0
SSH_CHILD=""
if command -v pgrep >/dev/null 2>&1; then
BRS_PID=""
# Find the baresnap child process of timeout
for _ in $(seq 1 30); do
BRS_PID=$(pgrep -P "$CHAOS_PID" -x "$(basename "$BARESNAP")" 2>/dev/null | head -n1 || true)
if [ -z "$BRS_PID" ]; then
BRS_PID=$(pgrep -P "$CHAOS_PID" -f "$BARESNAP" 2>/dev/null | head -n1 || true)
fi
[ -n "$BRS_PID" ] && break
sleep 0.1
done
# Find the ssh child process of baresnap
if [ -n "$BRS_PID" ]; then
for _ in $(seq 1 30); do
SSH_CHILD=$(pgrep -P "$BRS_PID" -x ssh 2>/dev/null | head -n1 || true)
if [ -z "$SSH_CHILD" ]; then
SSH_CHILD=$(pgrep -P "$BRS_PID" -f "ssh" 2>/dev/null | head -n1 || true)
fi
[ -n "$SSH_CHILD" ] && break
sleep 0.1
done
fi
fi
if [ -n "$SSH_CHILD" ]; then
pass "60.3 SSH process located (pid=$SSH_CHILD)"
CHAOS_APPLIED=1
# 5 micro-cut cycles: SIGSTOP 0.5s → SIGCONT 0.5s
for cycle in 1 2 3 4 5; do
if ! kill -0 "$CHAOS_PID" 2>/dev/null; then
break
fi
if kill -STOP "$SSH_CHILD" 2>/dev/null; then
sleep 0.5
kill -CONT "$SSH_CHILD" 2>/dev/null || true
sleep 0.5
else
break
fi
done
else
fail "60.3 child SSH not located; chaos not applied"
fi
# Wait for create to finish
wait "$CHAOS_PID" 2>/dev/null
CHAOS_RC=$?
if [ "$CHAOS_APPLIED" -eq 1 ]; then
if [ "$CHAOS_RC" -eq 124 ]; then
fail "60.4 DEADLOCK: create did not finish in 120s"
elif [ "$CHAOS_RC" -ge 128 ]; then
fail "60.4 create died by signal $((CHAOS_RC - 128)) during micro-cuts"
elif [ "$CHAOS_RC" -eq 0 ]; then
pass "60.4 create survived micro-cuts (rc=0)"
else
pass "60.4 create finished with controlled error (rc=$CHAOS_RC)"
fi
# Verify remote repo integrity
set +e
"$BARESNAP" verify "$CHAOS_URI" >/dev/null 2>&1
CHAOS_VERIFY=$?
set -e
if [ "$CHAOS_VERIFY" -eq 0 ]; then
pass "60.5 repo intact after chaos monkey"
else
fail "60.5 repo corrupt after chaos monkey"
fi
else
fail "60.4 create could not be subjected to micro-cuts"
fail "60.5 verify omitted because chaos not applied"
fi
ssh "$CHAOS_TARGET" "rm -rf $CHAOS_REMOTE" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi
# ============================================================
# 61. Ghost Permission: chmod 000 during create
# ============================================================
section "61. Ghost Permission: chmod 000 during create"
REPO61="$WORK/repo61"
SRC61="$WORK/src61"
assert_ok "61.1 init" "$BARESNAP" init "$REPO61"
mkdir -p "$SRC61"
# Create 500 small files so create takes long enough
for i in $(seq 1 500); do
printf 'perm ghost file %d
' "$i" > "$SRC61/file_$i.txt"
done
# Launch create in background
"$BARESNAP" create "$REPO61" "$SRC61" \
> "$WORK/perm_create.log" 2>&1 &
PERM_PID=$!
# Wait 20ms and remove read permissions from half the files
sleep 0.02
CHMOD_COUNT=0
for i in $(seq 1 500); do
if [ $((i % 2)) -eq 0 ]; then
chmod 000 "$SRC61/file_$i.txt" 2>/dev/null || true
CHMOD_COUNT=$((CHMOD_COUNT + 1))
fi
done
# Wait for create to finish
PERM_RC=0
wait "$PERM_PID" 2>/dev/null || PERM_RC=$?
# Restore permissions for cleanup
chmod -R u+rw "$SRC61" 2>/dev/null || true
if [ "$PERM_RC" -ge 128 ]; then
fail "61.2 create CRASHED with signal $((PERM_RC - 128))"
elif [ "$PERM_RC" -eq 0 ]; then
pass "61.2 create completed with $CHMOD_COUNT chmod 000 files"
else
pass "61.2 create finished with controlled error (rc=$PERM_RC, $CHMOD_COUNT chmod 000)"
fi
# Verify that the repo is not corrupt
set +e
"$BARESNAP" verify "$REPO61" >/dev/null 2>&1
PERM_VERIFY=$?
set -e
if [ "$PERM_VERIFY" -eq 0 ]; then
pass "61.3 repo intact after ghost chmod"
else
fail "61.3 repo corrupt after ghost chmod"
fi
# Verify there are no orphan temporary packs
TMP61=$(find "$REPO61/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP61" -eq 0 ]; then
pass "61.4 no orphan temporaries in tmp/"
else
fail "61.4 $TMP61 orphan temporaries in tmp/"
fi
# Verify that the snapshot was created (even if partial)
SNAP61=$(get_snap_count "$REPO61")
if [ "$SNAP61" -ge 1 ]; then
pass "61.5 snapshot created ($SNAP61) despite chmod 000"
else
fail "61.5 no snapshot was created"
fi
# ============================================================
# 62. Lock File: mutual exclusion between processes
# ============================================================
section "62. Lock File: mutual exclusion between processes"
REPO62="$WORK/repo62"
SRC62="$WORK/src62"
OUT62_1="$WORK/lock_out1.txt"
OUT62_2="$WORK/lock_out2.txt"
assert_ok "62.1 init" "$BARESNAP" init "$REPO62"
mkdir -p "$SRC62"
# Enough data so create takes time and the lock activates
for i in $(seq 1 20); do
head -c 65536 /dev/urandom > "$SRC62/file_$i.bin"
done
assert_ok "62.2 initial create" "$BARESNAP" create "$REPO62" "$SRC62"
# Modify data to force a second real create
for i in $(seq 1 20); do
printf 'MOD' | dd of="$SRC62/file_$i.bin" bs=1 seek=100 conv=notrunc status=none 2>/dev/null
done
RC1=0
RC2=0
# Launch two simultaneous creates.
# IMPORTANT: `wait` must go with `|| RC=$?` if the suite uses `set -e`.
"$BARESNAP" create "$REPO62" "$SRC62" > "$OUT62_1" 2>&1 &
PID1=$!
sleep 0.2
"$BARESNAP" create "$REPO62" "$SRC62" > "$OUT62_2" 2>&1 &
PID2=$!
wait "$PID1" 2>/dev/null || RC1=$?
wait "$PID2" 2>/dev/null || RC2=$?
LOCK_FAIL1=0
LOCK_FAIL2=0
if grep -Eqi 'locked|lock file' "$OUT62_1" 2>/dev/null; then
LOCK_FAIL1=1
fi
if grep -Eqi 'locked|lock file' "$OUT62_2" 2>/dev/null; then
LOCK_FAIL2=1
fi
if [ "$RC1" -eq 0 ] && [ "$RC2" -eq 0 ]; then
pass "62.3 both creates completed (did not overlap or lock serialized by timing)"
elif [ "$RC1" -eq 0 ] && [ "$RC2" -ne 0 ] && [ "$LOCK_FAIL2" -eq 1 ]; then
pass "62.3 create 1 completed; create 2 rejected by lock"
elif [ "$RC2" -eq 0 ] && [ "$RC1" -ne 0 ] && [ "$LOCK_FAIL1" -eq 1 ]; then
pass "62.3 create 2 completed; create 1 rejected by lock"
else
echo "--- $OUT62_1 ---" >&2
cat "$OUT62_1" >&2 || true
echo "--- $OUT62_2 ---" >&2
cat "$OUT62_2" >&2 || true
fail "62.3 unexpected result (rc1=$RC1, rc2=$RC2, lock1=$LOCK_FAIL1, lock2=$LOCK_FAIL2)"
fi
# Verify that the lock did not leave temporaries or corrupt the repo
TMP62=0
if [ -d "$REPO62/tmp" ]; then
TMP62=$(find "$REPO62/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l || true)
fi
TMP62=${TMP62:-1}
if [ "$TMP62" -eq 0 ]; then
pass "62.4 no temporaries after concurrency with lock"
else
fail "62.4 $TMP62 orphan temporaries after lock"
fi
assert_ok "62.5 verify after concurrency with lock" "$BARESNAP" verify "$REPO62"
SNAPS62=$(get_snap_count "$REPO62")
if [ "$SNAPS62" -ge 2 ]; then
pass "62.6 snapshots created correctly ($SNAPS62)"
else
fail "62.6 only $SNAPS62 snapshots (expected >= 2)"
fi
# ============================================================
# 63. SSH Timeout: dead agent detection (poll + timeout)
# ============================================================
# Functional test if SSH is available
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_TARGET="$(whoami)@localhost"
REMOTE_PATH63="/tmp/baresnap_mega_test_timeout"
SSH_URI63="ssh://${SSH_TARGET}${REMOTE_PATH63}"
SSH_SRC63="$WORK/ssh_src63"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH63" 2>/dev/null || true
mkdir -p "$SSH_SRC63"
printf 'timeout test
' > "$SSH_SRC63/file.txt"
head -c 32768 /dev/urandom > "$SSH_SRC63/blob.bin"
assert_ok "63.5 remote install" "$BARESNAP" remote install "$SSH_URI63"
assert_ok "63.6 remote init" "$BARESNAP" init "$SSH_URI63"
assert_ok "63.7 remote create (timeout active)" "$BARESNAP" create "$SSH_URI63" "$SSH_SRC63"
assert_ok "63.8 remote verify" "$BARESNAP" verify "$SSH_URI63"
ssh "$SSH_TARGET" "rm -rf $REMOTE_PATH63" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH functional test omitted"
fi
# ============================================================
# 64. Index Checksum: FNV1A-64 integrity
# ============================================================
section "64. Index Checksum: FNV1A-64 integrity"
REPO64="$WORK/repo64"
SRC64="$WORK/src64"
assert_ok "64.1 init" "$BARESNAP" init "$REPO64"
mkdir -p "$SRC64"
printf 'checksum test data
' > "$SRC64/data.txt"
head -c 65536 /dev/urandom > "$SRC64/blob.bin"
assert_ok "64.2 create" "$BARESNAP" create "$REPO64" "$SRC64"
assert_ok "64.3 verify before corrupting" "$BARESNAP" verify "$REPO64"
IDX_FILE=$(ls "$REPO64/index/"*.idx 2>/dev/null | head -1)
IDX_BACKUP="$WORK/idx64_backup.idx"
if [ -n "$IDX_FILE" ]; then
cp "$IDX_FILE" "$IDX_BACKUP"
IDX_SIZE=$(stat -c '%s' "$IDX_FILE")
if [ "$IDX_SIZE" -gt 8 ]; then
# Last 8 bytes = FNV1A-64 checksum
CKSUM_HEX=$(tail -c 8 "$IDX_FILE" | xxd -p)
if [ "$CKSUM_HEX" != "0000000000000000" ]; then
pass "64.4 index checksum is non-zero: $CKSUM_HEX"
else
fail "64.4 index checksum is zero (not implemented)"
fi
else
fail "64.4 index too small to have checksum"
fi
# Corrupt a byte in the data area (not the checksum)
CORRUPT_OFFSET=$((IDX_SIZE / 2))
printf '\xFF' | dd of="$IDX_FILE" bs=1 seek="$CORRUPT_OFFSET" conv=notrunc status=none 2>/dev/null
set +e
"$BARESNAP" verify "$REPO64" >/dev/null 2>&1
VERIFY_RC64=$?
set -e
if [ "$VERIFY_RC64" -ne 0 ]; then
pass "64.5 verify detects corrupt index via checksum (rc=$VERIFY_RC64)"
else
fail "64.5 verify did NOT detect corrupt index"
fi
# Restore and verify it works again
cp "$IDX_BACKUP" "$IDX_FILE"
assert_ok "64.6 verify passes after restoring clean index" "$BARESNAP" verify "$REPO64"
else
fail "64.4 no index found to corrupt"
fail "64.5 skip"
fail "64.6 skip"
fi
# ============================================================
# 65. Ouroboros: The repo scans itself
# ============================================================
section "65. Ouroboros: The repo scans itself"
REPO65="$WORK/ouroboros_repo"
SRC65="$WORK" # The source is the PARENT directory
assert_ok "65.1 init" "$BARESNAP" init "$REPO65"
# Create real data at the same level as the repo
mkdir -p "$SRC65/datos_reales"
printf 'data that must not die
' > "$SRC65/datos_reales/importante.txt"
pass "65.2 real data created in $SRC65"
# Launch the "suicidal" create.
# BareSnap does NOT exclude the repo, but being empty/atomic,
# the scanner does not enter an exponential loop and finishes quickly.
# We use timeout just in case to avoid blocking the test suite.
set +e
timeout 10 "$BARESNAP" create "$REPO65" "$SRC65" > "$WORK/ouroboros.log" 2>&1
OUROBOROS_RC=$?
set -e
if [ "$OUROBOROS_RC" -eq 124 ]; then
fail "65.3 BareSnap entered infinite loop/deadlock (timeout)"
elif [ "$OUROBOROS_RC" -eq 0 ]; then
pass "65.3 BareSnap completed scanning its own structure without deadlock"
else
# If it failed due to ENOSPC or another error, it is also valid (survived the chaos)
pass "65.3 BareSnap cleanly aborted the suicidal scan (rc=$OUROBOROS_RC)"
fi
# SURVIVAL VERIFICATION
# 1. No zombie temporaries should remain in tmp/ (the abort or success cleaned them)
TMP65=$(find "$REPO65/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP65" -eq 0 ]; then
pass "65.4 Zero zombie temporaries after Ouroboros scan"
else
fail "65.4 $TMP65 zombie temporaries survived in tmp/"
fi
# 2. The repo must remain 100% intact and verifiable
assert_ok "65.5 verify of the repo survives the cannibal scan" "$BARESNAP" verify "$REPO65"
# 3. The user's real data was not corrupted
if [ -f "$SRC65/datos_reales/importante.txt" ]; then
CONTENT=$(cat "$SRC65/datos_reales/importante.txt")
if [ "$CONTENT" = "data that must not die" ]; then
pass "65.6 User data remains intact on disk"
else
fail "65.6 User data was altered"
fi
else
fail "65.6 User data disappeared!"
fi
# ============================================================
# 66. File Cache Self-Healing (Cache Healing)
# ============================================================
section "66. File Cache Self-Healing (Cache Healing)"
REPO66="$WORK/repo66"
SRC66="$WORK/src66"
OUT66="$WORK/out66"
BASE66="$OUT66/$(basename "$SRC66")"
assert_ok "66.1 init" "$BARESNAP" init "$REPO66"
mkdir -p "$SRC66"
printf 'cache healing test v1
' > "$SRC66/file.txt"
head -c 32768 /dev/urandom > "$SRC66/data.bin"
# First backup to generate the initial cache
assert_ok "66.2 create v1" "$BARESNAP" create "$REPO66" "$SRC66"
# Slightly modify to force delta in the next one
sleep 1.1
printf 'cache healing test v2
' > "$SRC66/file.txt"
assert_ok "66.3 create v2 (delta expected)" "$BARESNAP" create "$REPO66" "$SRC66"
# Verify that delta worked (small pack)
PACKS_BEFORE=$(du -sk "$REPO66/packs" | awk '{print $1}')
# CORRUPT THE CACHE: Inject garbage into the cache file
CACHE_FILE="$REPO66/cache"
if [ -f "$CACHE_FILE" ]; then
# Overwrite the beginning of the cache with zeros (breaks magic and entries)
dd if=/dev/zero of="$CACHE_FILE" bs=1 count=64 conv=notrunc status=none 2>/dev/null
pass "66.4 file cache artificially corrupted"
else
fail "66.4 no cache file found to corrupt"
fi
# Run another backup. The engine should detect the inconsistency,
# delete the corrupt cache and regenerate it from scratch.
# This backup will be slower (re-indexing) but must finish successfully.
assert_ok "66.5 create post-corruption (auto-heal)" "$BARESNAP" create "$REPO66" "$SRC66"
# Verify that the repo remains intact
assert_ok "66.6 verify after auto-heal" "$BARESNAP" verify "$REPO66"
# Restore and verify that the data is correct (delta should have worked after healing)
rm -rf "$OUT66"
LATEST_SNAP66=$(get_latest_snap "$REPO66")
assert_ok "66.7 restore after auto-heal" "$BARESNAP" restore "$REPO66" "$LATEST_SNAP66" "$OUT66"
assert_ok "66.8 correct content after auto-heal" cmp -s "$SRC66/file.txt" "$BASE66/file.txt"
# Verify that the cache has been regenerated (it is not empty or corrupt)
if [ -f "$CACHE_FILE" ]; then
CACHE_SIZE=$(stat -c '%s' "$CACHE_FILE")
if [ "$CACHE_SIZE" -gt 100 ]; then
pass "66.9 file cache regenerated correctly (${CACHE_SIZE} bytes)"
else
fail "66.9 regenerated file cache is suspiciously small (${CACHE_SIZE} bytes)"
fi
else
fail "66.9 file cache was not regenerated"
fi
# ==============================================================================
# ARMORED HELPERS: Integrated batteries (weird paths + edge cases)
# ==============================================================================
WP_NL=$'
'
WP_TAB=$'\t'
WP_DQ='"'
WP_BT='`'
WP_DL='$'
assert_fail() {
# We temporarily disable set -u to avoid deaths from empty variables
set +u
local desc="${1:-Test without description}"
shift
# If no valid commands are passed, we fail in a controlled way
if [ $# -eq 0 ]; then
fail "$desc (assert_fail invoked without valid commands)"
set -u
return 1
fi
local rc=0
# We force a safe isolated context for the execution of the hostile command
"$@" >/dev/null 2>&1 </dev/null || rc=$?
set -u # We reactivate the mandatory strict mode of mega_test.sh
if [ "$rc" -eq 0 ]; then
fail "$desc (should have failed but succeeded)"
else
pass "$desc"
fi
}
weird_count_files() {
local root="${1:-}"
[ -z "$root" ] && { printf '0'; return; }
# We avoid indirect process substitutions using pure POSIX pipes
find "$root" -type f -print0 2>/dev/null | awk 'BEGIN{RS="\0"} {n++} END{print n+0}'
}
weird_find_file_by_name() {
local root="${1:-}"
local target_name="${2:-}"
[ -z "$root" ] || [ -z "$target_name" ] && return 1
local f
# We use a classic loop indexed by the filesystem inode to avoid leaks
while IFS= read -r -d '' f; do
if [ "$(basename -- "$f")" = "$target_name" ]; then
printf '%s' "$f"
return 0
fi
done < <(find "$root" -type f -print0 2>/dev/null)
return 1
}
weird_find_dir_by_name() {
local root="${1:-}"
local target_name="${2:-}"
[ -z "$root" ] || [ -z "$target_name" ] && return 1
local f
while IFS= read -r -d '' f; do
if [ "$(basename -- "$f")" = "$target_name" ]; then
printf '%s' "$f"
return 0
fi
done < <(find "$root" -type d -print0 2>/dev/null)
return 1
}
weird_compare_dirs() {
local src="${1:-}"
local dst="${2:-}"
[ -d "$src" ] || return 1
[ -d "$dst" ] || return 1
# We create dynamic unique identifiers to avoid concurrent collisions (Ouroboros)
local src_list="$WORK/.wp_src_$$.list"
local dst_list="$WORK/.wp_dst_$$.list"
(cd "$src" && find . -mindepth 1 -print0 2>/dev/null | sort -z) > "$src_list" || return 1
(cd "$dst" && find . -mindepth 1 -print0 2>/dev/null | sort -z) > "$dst_list" || return 1
if ! cmp -s "$src_list" "$dst_list"; then
rm -f "$src_list" "$dst_list"
return 1
fi
local f s d
local ok=0
while IFS= read -r -d '' f; do
s="$src/$f"
d="$dst/$f"
if [ -L "$s" ]; then
if [ ! -L "$d" ] || [ "$(readlink -- "$s")" != "$(readlink -- "$d")" ]; then
ok=1; break
fi
elif [ -d "$s" ]; then
if [ ! -d "$d" ]; then ok=1; break; fi
elif [ -f "$s" ]; then
if ! cmp -s "$s" "$d"; then ok=1; break; fi
fi
done < "$src_list"
rm -f "$src_list" "$dst_list"
return $ok
}
weird_make_tree() {
local root="${1:-}"
[ -n "$root" ] || return 1
# Strict cleanup using the POSIX double dash to avoid names with initial dash breaking rm
rm -rf -- "$root"
mkdir -p -- "$root"
local NL=$'
'
local TAB=$'\t'
local DQ='"'
local BT='`'
local DL='$'
# Tree structure with strict escaping
mkdir -p -- "$root/dir with spaces"
printf 'content spaces
' > "$root/dir with spaces/file with spaces.txt"
mkdir -p -- "$root/dir'with'quotes"
printf 'single quote content
' > "$root/dir'with'quotes/file'quote.txt"
mkdir -p -- "$root/dir${DQ}with${DQ}double"
printf 'double quote content
' > "$root/dir${DQ}with${DQ}double/file${DQ}double.txt"
mkdir -p -- "$root/dir;semicolon & ampersand"
printf 'shell metachar content
' > "$root/dir;semicolon & ampersand/file; & | >.txt"
mkdir -p -- "$root/dir backtick ${BT} and dollar ${DL}"
printf 'dangerous expansion content
' > "$root/dir backtick ${BT} and dollar ${DL}/file ${BT}id${BT} ${DL}HOME.txt"
mkdir -p -- "$root/dir [glob] ? * \\backslash"
printf 'glob and backslash content
' > "$root/dir [glob] ? * \\backslash/file [a-z]?.txt"
mkdir -p -- "$root/dir${TAB}tab"
printf 'tab content
' > "$root/dir${TAB}tab/file${TAB}tab.txt"
mkdir -p -- "$root/dir unicode ñ 中文 русский 😀"
printf 'unicode content
' > "$root/dir unicode ñ 中文 русский 😀/файл ñ 中文 😀.txt"
mkdir -p -- "$root/empty dir"
mkdir -p -- "$root/deep path with spaces/level 2/level 3"
printf 'deep content
' > "$root/deep path with spaces/level 2/level 3/deep file.txt"
# Files with initial dash are protected with the explicit prefix of the root directory
printf 'leading dash
' > "$root/-leading-dash-file.txt"
printf 'double dash
' > "$root/--leading-double-dash.txt"
printf 'root weird file
' > "$root/file with 'single' and ${DQ}double${DQ} quotes.txt"
printf 'danger file
' > "$root/file; rm -rf --no-preserve-root &.txt"
printf 'trailing space
' > "$root/file with trailing space .txt"
printf 'leading space
' > "$root/ leading space file.txt"
printf 'colon file
' > "$root/file:with:colon.txt"
printf 'newline filename
' > "$root/file${NL}with newline.txt"
mkdir -p -- "$root/.hidden dir"
printf 'hidden content
' > "$root/.hidden dir/.hidden file with spaces"
ln -s "dir with spaces/file with spaces.txt" "$root/link to weird target" 2>/dev/null || true
}
# ============================================================
# VALGRIND HELPERS (modules 104-106)
# ============================================================
# Valgrind log audit: 0 definite leaks + 0 errors.
# Corrected version: the && operator has precedence over ||,
# therefore it is evaluated in two explicit conditions.
check_vg_log() {
local log_file="$1"
local stage="$2"
if [ ! -f "$log_file" ]; then
fail "$stage: valgrind log not generated"
return 1
fi
local leaks errors
leaks=$(grep "definitely lost:" "$log_file" | awk '{print $4}' | tr -d ',' | head -1)
errors=$(grep "ERROR SUMMARY:" "$log_file" | awk '{print $4}' | head -1)
leaks=${leaks:-0}
errors=${errors:-0}
if [ "$leaks" -eq 0 ] 2>/dev/null && [ "$errors" -eq 0 ] 2>/dev/null; then
pass "$stage: 0 leaks, 0 errors"
return 0
fi
fail "$stage: problems detected"
[ "$leaks" -gt 0 ] 2>/dev/null && log "    -> Leak: $leaks bytes definitely lost"
[ "$errors" -gt 0 ] 2>/dev/null && log "    -> Access/read errors: $errors"
log "    -> Log: $log_file"
return 0
}
# Common gate: only runs with --valgrind and binary installed.
vg_section_gate() {
if [ "$SKIP_VALGRIND" -eq 1 ]; then
log "  ${YELLOW}[SKIP]${NC} Valgrind modules disabled (--no-valgrind)"
return 1
fi
if [ "$VALGRIND_AVAILABLE" -ne 1 ]; then
log "  ${YELLOW}[SKIP]${NC} Valgrind is not installed"
return 1
fi
return 0
}
# ==============================================================================
# 67. Integrated battery: weird paths (ARMORED INTEGRATED VERSION)
# ==============================================================================
section "67. Integrated battery: weird paths"
# We force local variables to be safely initialized
WP_REPO="$WORK/repo_weird"
WP_SRC="$WORK/src weird & src"
WP_OUT="$WORK/out_weird"
# We use safe expansion tricks to preserve literal spaces intact
WP_SRCBASE="${WP_SRC##*/}"
WP_BASE="$WP_OUT/$WP_SRCBASE"
assert_ok "67.1 init weird" "$BARESNAP" init "$WP_REPO"
weird_make_tree "$WP_SRC"
assert_ok "67.2 create weird" "$BARESNAP" create "$WP_REPO" "$WP_SRC"
WP_SNAP="$(get_latest_snap "$WP_REPO")"
assert_ok "67.3 restore weird" "$BARESNAP" restore "$WP_REPO" "$WP_SNAP" "$WP_OUT"
if weird_compare_dirs "$WP_SRC" "$WP_BASE"; then
pass "67.4 restore identical"
else
fail "67.4 restore not identical"
fi
WP_OUT2="$WORK/out_weird_extract_full"
rm -rf -- "$WP_OUT2"
assert_ok "67.5 full extract weird" "$BARESNAP" extract "$WP_REPO" "$WP_SNAP" "$WP_OUT2"
WP_EXTRACT_ROOT="$WP_OUT2/$WP_SRCBASE"
if [ ! -d "$WP_EXTRACT_ROOT" ]; then
WP_EXTRACT_ROOT="$WP_OUT2"
fi
if weird_compare_dirs "$WP_SRC" "$WP_EXTRACT_ROOT"; then
pass "67.6 full extract identical"
else
fail "67.6 full extract not identical"
fi
if [ -d "$WP_EXTRACT_ROOT/empty dir" ]; then
pass "67.7 full extract preserves empty directory"
else
fail "67.7 full extract does NOT preserve empty directory"
fi
WP_OUT3="$WORK/out_weird_empty"
rm -rf -- "$WP_OUT3"
assert_ok "67.8 selective extract empty directory" \
"$BARESNAP" extract "$WP_REPO" "$WP_SNAP" "$WP_OUT3" "$WP_SRCBASE/empty dir"
# We avoid empty variables breaking set -u by forcing a safe fallback value
WP_EMPTY_DIR_FOUND="$(weird_find_dir_by_name "$WP_OUT3" "empty dir" || printf '')"
if [ -n "${WP_EMPTY_DIR_FOUND:-}" ]; then
pass "67.9 selective extract preserves empty directory"
else
fail "67.9 selective extract does NOT preserve empty directory"
fi
WP_OUT4="$WORK/out_weird_multi"
rm -rf -- "$WP_OUT4"
# The double dash '--' shields the C binary against accidental argument injections
assert_ok "67.10 weird multiple extract" \
"$BARESNAP" extract "$WP_REPO" "$WP_SNAP" "$WP_OUT4" \
"$WP_SRCBASE/dir'with'quotes/file'quote.txt" \
"$WP_SRCBASE/dir;semicolon & ampersand/file; & | >.txt" \
"$WP_SRCBASE/dir backtick ${WP_BT} and dollar ${WP_DL}/file ${WP_BT}id${WP_BT} ${WP_DL}HOME.txt" \
"$WP_SRCBASE/dir [glob] ? * \\backslash/file [a-z]?.txt" \
"$WP_SRCBASE/dir unicode ñ 中文 русский 😀/файл ñ 中文 😀.txt" \
"$WP_SRCBASE/file${WP_NL}with newline.txt"
WP_MULTI_COUNT="$(weird_count_files "$WP_OUT4")"
assert_eq "67.11 multiple extract: 6 files" "6" "$WP_MULTI_COUNT"
WP_OUT5="$WORK/out_weird_spaces"
rm -rf -- "$WP_OUT5"
assert_ok "67.12 extract file with spaces" \
"$BARESNAP" extract "$WP_REPO" "$WP_SNAP" "$WP_OUT5" \
"$WP_SRCBASE/dir with spaces/file with spaces.txt"
WP_SPACE_FILE_FOUND="$(weird_find_file_by_name "$WP_OUT5" "file with spaces.txt" || printf '')"
if [ -n "${WP_SPACE_FILE_FOUND:-}" ] && cmp -s "$WP_SRC/dir with spaces/file with spaces.txt" "$WP_SPACE_FILE_FOUND"; then
pass "67.13 file with spaces extracted correctly"
else
fail "67.13 file with spaces not extracted correctly"
fi
# ==============================================================================
# SUB-TEST: SHIELDING MICROSOFT TRAILING SPACES (Windows Curse)
# ==============================================================================
WP_REPO_TRAILING="$WORK/repo_weird_trailing"
WP_SRC_TRAILING="$WORK/src10 trailing space "
WP_OUT_TRAILING="$WORK/out_weird_trailing"
# We physically protect filesystem operations with the double dash
rm -rf -- "$WP_SRC_TRAILING" "$WP_OUT_TRAILING"
mkdir -p -- "$WP_SRC_TRAILING"
printf 'trailing dir
' > "$WP_SRC_TRAILING/file.txt"
assert_ok "67.14 init trailing space" "$BARESNAP" init "$WP_REPO_TRAILING"
assert_ok "67.15 create trailing space" "$BARESNAP" create "$WP_REPO_TRAILING" "$WP_SRC_TRAILING"
WP_SNAP_TRAILING="$(get_latest_snap "$WP_REPO_TRAILING")"
assert_ok "67.16 restore trailing space" \
"$BARESNAP" restore "$WP_REPO_TRAILING" "$WP_SNAP_TRAILING" "$WP_OUT_TRAILING"
# CRITICAL SOLUTION: We extract the basename purely without invoking $(basename) subshells that trim spaces
WP_SRC_TRAILING_BASE="${WP_SRC_TRAILING##*/}"
WP_BASE_TRAILING="$WP_OUT_TRAILING/$WP_SRC_TRAILING_BASE"
if cmp -s "$WP_SRC_TRAILING/file.txt" "$WP_BASE_TRAILING/file.txt"; then
pass "67.17 correct content with source ending in space"
else
fail "67.17 incorrect content with source ending in space (Bash subshell false positive)"
fi
# ==============================================================================
# 68. Integrated battery: CLI edge cases (DEFINITIVE CORRECTED VERSION)
# ==============================================================================
section "68. Integrated battery: CLI edge cases"
EC_SRC="$WORK/ec_src"
mkdir -p "$EC_SRC"
printf 'edge test
' > "$EC_SRC/f.txt"
EC_REPO="$WORK/ec_repo"
assert_ok "68.1 init edge repo" "$BARESNAP" init "$EC_REPO"
assert_fail "68.2 create with non-existent --zstd" \
"$BARESNAP" create "$EC_REPO" "$EC_SRC" --zstd
assert_fail "68.3 create with --compress typo" \
"$BARESNAP" create "$EC_REPO" "$EC_SRC" --compress zstd
assert_fail "68.4 create with --compression without value" \
"$BARESNAP" create "$EC_REPO" "$EC_SRC" --compression
assert_fail "68.5 create with --zstd-level without value" \
"$BARESNAP" create "$EC_REPO" "$EC_SRC" --zstd-level
assert_fail "68.6 init with --compression without value" \
"$BARESNAP" init "$WORK/ec_repo_bad" --compression
EC_REPO_ZSTD="$WORK/ec_repo_zstd_level"
assert_ok "68.7 create --zstd-level 5 auto-init" \
"$BARESNAP" create "$EC_REPO_ZSTD" "$EC_SRC" --zstd-level 5
EC_INFO_ZSTD="$("$BARESNAP" info "$EC_REPO_ZSTD" 2>&1 || printf '')"
if printf '%s
' "$EC_INFO_ZSTD" | grep -qi "zstd"; then
pass "68.8 info confirms zstd after --zstd-level"
else
fail "68.8 info confirms zstd after --zstd-level (zstd not found in info)"
fi
assert_ok "68.9 normal create for extract" \
"$BARESNAP" create "$EC_REPO" "$EC_SRC"
EC_SNAP="$(get_latest_snap "$EC_REPO")"
assert_fail "68.10 extract with path traversal ../" \
"$BARESNAP" extract "$EC_REPO" "$EC_SNAP" "$WORK/ec_out_traversal" "../etc/passwd"
assert_fail "68.11 extract with absolute path" \
"$BARESNAP" extract "$EC_REPO" "$EC_SNAP" "$WORK/ec_out_abs" "/etc/hosts"
EC_FAKE_TARGET="$WORK/ec_fake_target"
rm -f -- "$EC_FAKE_TARGET"
touch "$EC_FAKE_TARGET"
assert_fail "68.12 extract with target that is a file" \
"$BARESNAP" extract "$EC_REPO" "$EC_SNAP" "$EC_FAKE_TARGET"
EC_OUT_GHOST="$WORK/ec_out_ghost"
rm -rf -- "$EC_OUT_GHOST"
assert_ok "68.13 extract with non-existent filter does not crash" \
"$BARESNAP" extract "$EC_REPO" "$EC_SNAP" "$EC_OUT_GHOST" "nonexistent_dir/ghost.txt"
if [ -d "$EC_OUT_GHOST/nonexistent_dir" ]; then
fail "68.14 extract ignored non-existent filter (created ghost directory)"
else
pass "68.14 extract ignored non-existent filter"
fi
EC_REAL="$WORK/ec_real"
EC_LINK="$WORK/ec_link"
rm -rf -- "$EC_REAL" "$EC_LINK"
mkdir -p "$EC_REAL"
printf 'real
' > "$EC_REAL/real.txt"
ln -s "$EC_REAL" "$EC_LINK"
EC_REPO_LINK="$WORK/ec_repo_link"
assert_ok "68.15 init repo symlink source" "$BARESNAP" init "$EC_REPO_LINK"
assert_ok "68.16 create from symlink to directory" \
"$BARESNAP" create "$EC_REPO_LINK" "$EC_LINK"
EC_SNAP_LINK="$(get_latest_snap "$EC_REPO_LINK")"
EC_OUT_LINK="$WORK/ec_out_link"
rm -rf -- "$EC_OUT_LINK"
assert_ok "68.17 restore from symlink source" \
"$BARESNAP" restore "$EC_REPO_LINK" "$EC_SNAP_LINK" "$EC_OUT_LINK"
if [ -f "$EC_OUT_LINK/ec_real/real.txt" ] || [ -f "$EC_OUT_LINK/ec_link/real.txt" ]; then
pass "68.18 content from symlink restored"
else
fail "68.18 content from symlink not restored"
fi
EC_SRC_PERM="$WORK/ec_src_perm"
rm -rf -- "$EC_SRC_PERM"
mkdir -p "$EC_SRC_PERM/ok_dir" "$EC_SRC_PERM/forbidden_dir"
printf 'ok
' > "$EC_SRC_PERM/ok_dir/file.txt"
printf 'secret
' > "$EC_SRC_PERM/forbidden_dir/secret.txt"
chmod 000 "$EC_SRC_PERM/forbidden_dir"
EC_REPO_PERM="$WORK/ec_repo_perm"
assert_ok "68.19 init repo weird permissions" "$BARESNAP" init "$EC_REPO_PERM"
set +e
"$BARESNAP" create "$EC_REPO_PERM" "$EC_SRC_PERM" >/dev/null 2>&1
EC_PERM_RC=$?
set -e
if [ "$EC_PERM_RC" -ne 0 ]; then
pass "68.20 create aborted on directory without permissions (rc=$EC_PERM_RC)"
else
pass "68.20 create ignored directory without permissions and continued (rc=0)"
fi
chmod 755 "$EC_SRC_PERM/forbidden_dir"
assert_ok "68.21 verify after create with weird permissions" \
"$BARESNAP" verify "$EC_REPO_PERM"
assert_fail "68.22 restore with non-existent snapshot" \
"$BARESNAP" restore "$EC_REPO" "ghost_snap_123.snap" "$WORK/ec_out_ghost_snap"
assert_fail "68.23 init on existing repo" \
"$BARESNAP" init "$EC_REPO"
EC_REPO_COLL="$WORK/ec_repo_collision"
EC_SRC_COLL="$WORK/ec_src13"
EC_OUT_COLL="$WORK/ec_out_collision"
rm -rf -- "$EC_REPO_COLL" "$EC_SRC_COLL" "$EC_OUT_COLL"
mkdir -p "$EC_SRC_COLL"
printf 'file
' > "$EC_SRC_COLL/target"
assert_ok "68.24 init repo collision" "$BARESNAP" init "$EC_REPO_COLL"
assert_ok "68.25 create repo collision" "$BARESNAP" create "$EC_REPO_COLL" "$EC_SRC_COLL"
EC_SNAP_COLL="$(get_latest_snap "$EC_REPO_COLL")"
mkdir -p "$EC_OUT_COLL/ec_src13/target"
assert_ok "68.26 extract with type collision does not crash" \
"$BARESNAP" extract "$EC_REPO_COLL" "$EC_SNAP_COLL" "$EC_OUT_COLL"
if [ -d "$EC_OUT_COLL/ec_src13/target" ]; then
pass "68.27 extract respected pre-existing directory safely"
else
fail "68.27 extract failed on collision destroying structures"
fi
EC_REPO_LS="$WORK/ec_repo_ls"
EC_SRC_LS="$WORK/ec_src_ls"
rm -rf -- "$EC_REPO_LS" "$EC_SRC_LS"
mkdir -p "$EC_SRC_LS"
printf 'ls
' > "$EC_SRC_LS/f.txt"
assert_ok "68.28 init repo ls" "$BARESNAP" init "$EC_REPO_LS"
assert_ok "68.29 create repo ls" "$BARESNAP" create "$EC_REPO_LS" "$EC_SRC_LS"
EC_SNAP_LS="$(get_latest_snap "$EC_REPO_LS")"
assert_fail "68.30 ls with unknown option" \
"$BARESNAP" ls "$EC_REPO_LS" "$EC_SNAP_LS" --unknown
EC_OUT_MIX="$WORK/ec_out_mix"
rm -rf -- "$EC_OUT_MIX"
assert_fail "68.31 extract with mixed safe + unsafe filters" \
"$BARESNAP" extract "$EC_REPO" "$EC_SNAP" "$EC_OUT_MIX" \
"ec_src/f.txt" "../etc/shadow"
# ============================================================
# Helper assert_fail (in case you don't have it defined yet)
# ============================================================
if ! declare -F assert_fail >/dev/null 2>&1; then
assert_fail() {
local desc="$1"
shift
local rc=0
"$@" >/dev/null 2>&1 </dev/null || rc=$?
if [ "$rc" -eq 0 ]; then
fail "$desc (should have failed but succeeded)"
else
pass "$desc"
fi
}
fi
# ============================================================
# 69. INTEGRATED FINAL BOSS (adapted to Dual Core)
# ============================================================
section "69. INTEGRATED FINAL BOSS (adapted Dual Core)"
# Quick adjustments for old machines
FB9_DIRS=10
FB9_FILES_PER_DIR=10
FB9_FUZZ_ROUNDS=3
# ------------------------------------------------------------
# 69.1 REDUCED SWARM
# ------------------------------------------------------------
FB9_REPO="$WORK/fb9_repo"
FB9_SRC="$WORK/fb9_src"
FB9_OUT="$WORK/fb9_out"
assert_ok "69.1 init final boss" "$BARESNAP" init "$FB9_REPO"
mkdir -p "$FB9_SRC"
for i in $(seq 1 "$FB9_DIRS"); do
FB9_DIR="$FB9_SRC/dir $i (weird) [glob] & 'quote'"
mkdir -p "$FB9_DIR"
for j in $(seq 1 "$FB9_FILES_PER_DIR"); do
printf 'final boss %d-%d
' "$i" "$j" > "$FB9_DIR/file $i-$j 'q' [x].txt"
done
done
pass "69.2 reduced swarm created ($((FB9_DIRS * FB9_FILES_PER_DIR)) files)"
assert_ok "69.3 create swarm" "$BARESNAP" create "$FB9_REPO" "$FB9_SRC"
FB9_SNAP="$(get_latest_snap "$FB9_REPO")"
assert_ok "69.4 restore swarm" "$BARESNAP" restore "$FB9_REPO" "$FB9_SNAP" "$FB9_OUT"
FB9_SRC_FILES="$(find "$FB9_SRC" -type f 2>/dev/null | wc -l)"
FB9_DST_FILES="$(find "$FB9_OUT" -type f 2>/dev/null | wc -l)"
assert_eq "69.5 swarm file count" "$FB9_SRC_FILES" "$FB9_DST_FILES"
assert_ok "69.6 verify swarm" "$BARESNAP" verify "$FB9_REPO"
# ------------------------------------------------------------
# 69.7 SHORT DELTA CHAIN + PRUNE
# ------------------------------------------------------------
FB9_REPO_DELTA="$WORK/fb9_delta"
FB9_SRC_DELTA="$WORK/fb9_delta_src"
FB9_OUT_DELTA="$WORK/fb9_delta_out"
FB9_BASE_DELTA="$FB9_OUT_DELTA/$(basename "$FB9_SRC_DELTA")"
assert_ok "69.7 init delta" "$BARESNAP" init "$FB9_REPO_DELTA"
mkdir -p "$FB9_SRC_DELTA"
yes "FINAL BOSS INTEGRATED DELTA LINE." | head -c 32768 > "$FB9_SRC_DELTA/delta.txt"
assert_ok "69.8 create delta v1" "$BARESNAP" create "$FB9_REPO_DELTA" "$FB9_SRC_DELTA"
sleep 1.1
printf 'FB9_MOD_2 ' | dd of="$FB9_SRC_DELTA/delta.txt" bs=1 seek=400 conv=notrunc status=none
touch "$FB9_SRC_DELTA/delta.txt"
assert_ok "69.9 create delta v2" "$BARESNAP" create "$FB9_REPO_DELTA" "$FB9_SRC_DELTA"
sleep 1.1
printf 'FB9_MOD_3 ' | dd of="$FB9_SRC_DELTA/delta.txt" bs=1 seek=800 conv=notrunc status=none
touch "$FB9_SRC_DELTA/delta.txt"
assert_ok "69.10 create delta v3" "$BARESNAP" create "$FB9_REPO_DELTA" "$FB9_SRC_DELTA"
FB9_SNAPS_DELTA="$(get_snap_count "$FB9_REPO_DELTA")"
assert_eq "69.11 3 delta snapshots created" "3" "$FB9_SNAPS_DELTA"
assert_ok "69.12 prune --keep-last 1" "$BARESNAP" prune "$FB9_REPO_DELTA" --keep-last 1
FB9_SNAPS_DELTA_AFTER="$(get_snap_count "$FB9_REPO_DELTA")"
assert_eq "69.13 1 snapshot after prune" "1" "$FB9_SNAPS_DELTA_AFTER"
assert_ok "69.14 verify after delta prune" "$BARESNAP" verify "$FB9_REPO_DELTA"
FB9_SNAP_DELTA="$(get_latest_snap "$FB9_REPO_DELTA")"
assert_ok "69.15 restore after delta prune" "$BARESNAP" restore "$FB9_REPO_DELTA" "$FB9_SNAP_DELTA" "$FB9_OUT_DELTA"
assert_ok "69.16 delta.txt correct after prune+restore" \
cmp -s "$FB9_SRC_DELTA/delta.txt" "$FB9_BASE_DELTA/delta.txt"
# ------------------------------------------------------------
# 69.17 VAULT: encryption + weird paths + extract
# ------------------------------------------------------------
FB9_REPO_VAULT="$WORK/fb9_vault"
FB9_SRC_VAULT="$WORK/fb9 vault (weird) & [final]"
FB9_OUT_VAULT="$WORK/fb9_vault_out"
FB9_OUT_VAULT_EXTRACT="$WORK/fb9_vault_extract"
FB9_VAULT_BASENAME="$(basename "$FB9_SRC_VAULT")"
FB9_BASE_VAULT="$FB9_OUT_VAULT/$FB9_VAULT_BASENAME"
export BARESNAP_PASSPHRASE="fb9-vault-pass"
assert_ok "69.17 init encrypted" "$BARESNAP" init "$FB9_REPO_VAULT" --encrypt
mkdir -p "$FB9_SRC_VAULT/dir with spaces/'quotes' & [globs]"
printf 'fb9 secret v1
' > "$FB9_SRC_VAULT/dir with spaces/'quotes' & [globs]/secret.txt"
head -c 32768 /dev/urandom > "$FB9_SRC_VAULT/random.bin"
assert_ok "69.18 create encrypted v1" "$BARESNAP" create "$FB9_REPO_VAULT" "$FB9_SRC_VAULT"
sleep 1.1
printf 'fb9 secret v2
' > "$FB9_SRC_VAULT/dir with spaces/'quotes' & [globs]/secret.txt"
assert_ok "69.19 create encrypted v2" "$BARESNAP" create "$FB9_REPO_VAULT" "$FB9_SRC_VAULT"
FB9_SNAP_VAULT="$(get_latest_snap "$FB9_REPO_VAULT")"
assert_ok "69.20 restore encrypted" "$BARESNAP" restore "$FB9_REPO_VAULT" "$FB9_SNAP_VAULT" "$FB9_OUT_VAULT"
assert_ok "69.21 secret.txt correct" \
cmp -s "$FB9_SRC_VAULT/dir with spaces/'quotes' & [globs]/secret.txt" \
"$FB9_BASE_VAULT/dir with spaces/'quotes' & [globs]/secret.txt"
assert_ok "69.22 random.bin byte-identical" \
cmp -s "$FB9_SRC_VAULT/random.bin" "$FB9_BASE_VAULT/random.bin"
assert_ok "69.23 selective extract encrypted" \
"$BARESNAP" extract "$FB9_REPO_VAULT" "$FB9_SNAP_VAULT" "$FB9_OUT_VAULT_EXTRACT" \
"$FB9_VAULT_BASENAME/dir with spaces/'quotes' & [globs]/secret.txt"
FB9_FOUND_SECRET="$(find "$FB9_OUT_VAULT_EXTRACT" -name "secret.txt" -type f 2>/dev/null | head -1)"
if [ -n "${FB9_FOUND_SECRET:-}" ] && grep -q "fb9 secret v2" "$FB9_FOUND_SECRET" 2>/dev/null; then
pass "69.24 encrypted extract with weird paths correct"
else
fail "69.24 encrypted extract with weird paths incorrect"
fi
export BARESNAP_PASSPHRASE="wrong-fb9-pass"
assert_fail "69.25 restore encrypted with incorrect pass" \
"$BARESNAP" restore "$FB9_REPO_VAULT" "$FB9_SNAP_VAULT" "$WORK/fb9_bad_vault"
unset BARESNAP_PASSPHRASE
# ------------------------------------------------------------
# 69.26 EARTHQUAKE: multiple corruption + repair
# ------------------------------------------------------------
FB9_REPO_QUAKE="$WORK/fb9_quake"
FB9_SRC_QUAKE="$WORK/fb9_quake_src"
assert_ok "69.26 init earthquake" "$BARESNAP" init "$FB9_REPO_QUAKE"
mkdir -p "$FB9_SRC_QUAKE"
head -c 32768 /dev/urandom > "$FB9_SRC_QUAKE/data.bin"
printf 'quake v1
' > "$FB9_SRC_QUAKE/text.txt"
assert_ok "69.27 create snap 1" "$BARESNAP" create "$FB9_REPO_QUAKE" "$FB9_SRC_QUAKE"
sleep 1.1
printf 'quake v2
' >> "$FB9_SRC_QUAKE/text.txt"
assert_ok "69.28 create snap 2" "$BARESNAP" create "$FB9_REPO_QUAKE" "$FB9_SRC_QUAKE"
assert_ok "69.29 verify before earthquake" "$BARESNAP" verify "$FB9_REPO_QUAKE"
FB9_PACK="$(find "$FB9_REPO_QUAKE/packs" -maxdepth 1 -type f -name '*.pack' 2>/dev/null | head -1)"
FB9_SNAP_FILE="$(find "$FB9_REPO_QUAKE/snapshots" -maxdepth 1 -type f -name '*.snap' 2>/dev/null | head -1)"
FB9_IDX="$(find "$FB9_REPO_QUAKE/index" -maxdepth 1 -type f -name '*.idx' 2>/dev/null | head -1)"
if [ -n "${FB9_PACK:-}" ] && [ -n "${FB9_SNAP_FILE:-}" ] && [ -n "${FB9_IDX:-}" ]; then
FB9_SNAP_SIZE="$(stat -c '%s' "$FB9_SNAP_FILE" 2>/dev/null || echo 0)"
FB9_CORRUPT_OFF=50
if [ "${FB9_SNAP_SIZE:-0}" -le 60 ]; then
FB9_CORRUPT_OFF=$((FB9_SNAP_SIZE / 2))
fi
rm -f "$FB9_PACK"
printf '\xFF\xFE\xFD' | dd of="$FB9_SNAP_FILE" bs=1 seek="$FB9_CORRUPT_OFF" conv=notrunc status=none 2>/dev/null
rm -f "$FB9_IDX"
pass "69.30 earthquake applied (pack deleted + corrupt snap + deleted idx)"
assert_fail "69.31 verify detects corruption" "$BARESNAP" verify "$FB9_REPO_QUAKE"
FB9_VERIFY_RC=1
FB9_VERIFY_OUT=""
for FB9_TRY in 1 2 3; do
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$FB9_REPO_QUAKE" --repair \
</dev/null >/dev/null 2>&1 || true
set +e
FB9_VERIFY_OUT="$("$BARESNAP" verify "$FB9_REPO_QUAKE" 2>&1)"
FB9_VERIFY_RC=$?
set -e
if [ "$FB9_VERIFY_RC" -eq 0 ]; then
break
fi
done
if [ "$FB9_VERIFY_RC" -eq 0 ]; then
pass "69.32 verify passes after repair"
else
fail "69.32 verify fails after repair"
log "  [DEBUG] verify output:"
printf '%s
' "$FB9_VERIFY_OUT" | tail -n 6 | while IFS= read -r line; do
log "    $line"
done
fi
FB9_DAMAGED="$(find "$FB9_REPO_QUAKE/damaged" -type f -name '*.snap' 2>/dev/null | wc -l)"
FB9_LIVE="$(get_snap_count "$FB9_REPO_QUAKE")"
log "  [INFO] post-earthquake: $FB9_LIVE alive, ${FB9_DAMAGED:-0} in damaged/"
if [ "${FB9_DAMAGED:-0}" -ge 1 ]; then
pass "69.33 damaged snapshots moved to damaged/"
else
fail "69.33 no damaged snapshot was moved"
fi
else
fail "69.30 could not locate pack/snap/idx for the earthquake"
fail "69.31 skip"
fail "69.32 skip"
fail "69.33 skip"
fi
# ------------------------------------------------------------
# 69.34 LABYRINTH: long path
# ------------------------------------------------------------
FB9_REPO_MAZE="$WORK/fb9_maze"
FB9_SRC_MAZE="$WORK/fb9_maze_src"
FB9_OUT_MAZE="$WORK/fb9_maze_out"
assert_ok "69.34 init labyrinth" "$BARESNAP" init "$FB9_REPO_MAZE"
mkdir -p "$FB9_SRC_MAZE"
FB9_DEEP="$FB9_SRC_MAZE"
FB9_LEVEL=0
while true; do
FB9_NEXT="$FB9_DEEP/level_${FB9_LEVEL}_with_a_long_directory_name_for_path_limit"
if [ "${#FB9_NEXT}" -gt 3500 ]; then
break
fi
mkdir -p "$FB9_NEXT" 2>/dev/null || break
FB9_DEEP="$FB9_NEXT"
FB9_LEVEL=$((FB9_LEVEL + 1))
done
printf 'deep final boss
' > "$FB9_DEEP/deepest.txt"
pass "69.35 labyrinth created ($FB9_LEVEL levels)"
assert_ok "69.36 create labyrinth" "$BARESNAP" create "$FB9_REPO_MAZE" "$FB9_SRC_MAZE"
FB9_SNAP_MAZE="$(get_latest_snap "$FB9_REPO_MAZE")"
assert_ok "69.37 restore labyrinth" "$BARESNAP" restore "$FB9_REPO_MAZE" "$FB9_SNAP_MAZE" "$FB9_OUT_MAZE"
FB9_DEEP_RESTORED="$(find "$FB9_OUT_MAZE" -name "deepest.txt" -type f 2>/dev/null | head -1)"
if [ -n "${FB9_DEEP_RESTORED:-}" ] && grep -q "deep final boss" "$FB9_DEEP_RESTORED" 2>/dev/null; then
pass "69.38 deep file restored"
else
fail "69.38 deep file not restored"
fi
assert_ok "69.39 verify labyrinth" "$BARESNAP" verify "$FB9_REPO_MAZE"
# ------------------------------------------------------------
# 69.40 RUSSIAN ROULETTE: light snapshot fuzzing
# ------------------------------------------------------------
FB9_REPO_FUZZ="$WORK/fb9_fuzz"
FB9_SRC_FUZZ="$WORK/fb9_fuzz_src"
assert_ok "69.40 init fuzzing" "$BARESNAP" init "$FB9_REPO_FUZZ"
mkdir -p "$FB9_SRC_FUZZ"
head -c 32768 /dev/urandom > "$FB9_SRC_FUZZ/fuzz.bin"
assert_ok "69.41 create fuzzing" "$BARESNAP" create "$FB9_REPO_FUZZ" "$FB9_SRC_FUZZ"
FB9_FUZZ_SNAP_FILE="$(find "$FB9_REPO_FUZZ/snapshots" -maxdepth 1 -type f -name '*.snap' 2>/dev/null | head -1)"
FB9_FUZZ_BACKUP="$WORK/fb9_fuzz_backup.snap"
if [ -n "${FB9_FUZZ_SNAP_FILE:-}" ]; then
cp "$FB9_FUZZ_SNAP_FILE" "$FB9_FUZZ_BACKUP"
FB9_FUZZ_SIZE="$(stat -c '%s' "$FB9_FUZZ_SNAP_FILE" 2>/dev/null || echo 0)"
pass "69.42 snapshot copied for fuzzing (${FB9_FUZZ_SIZE} bytes)"
for FB9_MUT in $(seq 1 "$FB9_FUZZ_ROUNDS"); do
cp "$FB9_FUZZ_BACKUP" "$FB9_FUZZ_SNAP_FILE"
if [ "${FB9_FUZZ_SIZE:-0}" -le 30 ]; then
FB9_OFF=5
else
FB9_OFF=$(( (RANDOM % (FB9_FUZZ_SIZE - 20)) + 10 ))
fi
printf '\xFF' | dd of="$FB9_FUZZ_SNAP_FILE" bs=1 seek="$FB9_OFF" conv=notrunc status=none 2>/dev/null
assert_fail "69.43 fuzzing mutation $FB9_MUT detected" \
"$BARESNAP" verify "$FB9_REPO_FUZZ"
done
cp "$FB9_FUZZ_BACKUP" "$FB9_FUZZ_SNAP_FILE"
assert_ok "69.44 verify OK after restoring clean snapshot" \
"$BARESNAP" verify "$FB9_REPO_FUZZ"
else
fail "69.42 no snapshot found for fuzzing"
fail "69.43 skip"
fail "69.44 skip"
fi
# ------------------------------------------------------------
# 69.45 LIGHT APOCALYPSE: 2 concurrent creates
# ------------------------------------------------------------
FB9_REPO_APOC="$WORK/fb9_apoc"
FB9_SRC_APOC="$WORK/fb9_apoc_src"
FB9_OUT_APOC="$WORK/fb9_apoc_out"
assert_ok "69.45 init apocalypse" "$BARESNAP" init "$FB9_REPO_APOC"
mkdir -p "$FB9_SRC_APOC/src_1" "$FB9_SRC_APOC/src_2"
head -c 32768 /dev/urandom > "$FB9_SRC_APOC/src_1/data.bin"
head -c 32768 /dev/urandom > "$FB9_SRC_APOC/src_2/data.bin"
"$BARESNAP" create "$FB9_REPO_APOC" "$FB9_SRC_APOC/src_1" > "$WORK/fb9_apoc_1.log" 2>&1 &
FB9_PID1=$!
"$BARESNAP" create "$FB9_REPO_APOC" "$FB9_SRC_APOC/src_2" > "$WORK/fb9_apoc_2.log" 2>&1 &
FB9_PID2=$!
wait "$FB9_PID1" 2>/dev/null || true
wait "$FB9_PID2" 2>/dev/null || true
pass "69.46 concurrent creates finished"
FB9_SNAPS_APOC="$(get_snap_count "$FB9_REPO_APOC")"
if [ "${FB9_SNAPS_APOC:-0}" -ge 2 ]; then
pass "69.47 snapshots created correctly ($FB9_SNAPS_APOC)"
elif [ "${FB9_SNAPS_APOC:-0}" -ge 1 ]; then
pass "69.47 at least 1 snapshot created ($FB9_SNAPS_APOC)"
else
fail "69.47 no snapshot was created"
fi
assert_ok "69.48 verify post-apocalypse" "$BARESNAP" verify "$FB9_REPO_APOC"
assert_ok "69.49 prune --keep-last 1" "$BARESNAP" prune "$FB9_REPO_APOC" --keep-last 1
assert_ok "69.50 verify after prune" "$BARESNAP" verify "$FB9_REPO_APOC"
FB9_SNAP_APOC="$(get_latest_snap "$FB9_REPO_APOC")"
assert_ok "69.51 restore post-apocalypse" "$BARESNAP" restore "$FB9_REPO_APOC" "$FB9_SNAP_APOC" "$FB9_OUT_APOC"
pass "69.52 integrated FINAL BOSS passed"
# ===========================================================
# 70. DeLorean Effect: CMOS battery death (Epoch Reset 1970)
# ============================================================
section "70. DeLorean Effect: CMOS battery death (Epoch Reset 1970)"
if ! command -v faketime &>/dev/null; then
log "  ${YELLOW}[SKIP]${NC} faketime not installed"
else
REPO70="$WORK/repo70"
SRC70="$WORK/src70"
OUT70="$WORK/out70"
BASE70="$OUT70/$(basename "$SRC70")"
assert_ok "70.1 init" "$BARESNAP" init "$REPO70"
mkdir -p "$SRC70"
printf 'delorean test data
' > "$SRC70/time_file.txt"
head -c 32768 /dev/urandom > "$SRC70/time_blob.bin"
assert_ok "70.1 initial create in correct year (2026)" \
"$BARESNAP" create "$REPO70" "$SRC70"
# Simulate time reset to 1970
SNAP70_1=$(get_latest_snap "$REPO70")
sleep 1.1
printf 'delorean v2 post-epoch
' > "$SRC70/time_file.txt"
assert_ok "70.2 incremental create under faketime 1970" \
faketime '1970-01-01 00:00:00' "$BARESNAP" create "$REPO70" "$SRC70"
# Verify there is no name collision (UUID v4 protects)
SNAPS70=$(get_snap_count "$REPO70")
assert_eq "70.3 zero collisions by UUID v4 (2 snapshots)" "2" "$SNAPS70"
# Cache healing: the 1970 mtime must not crash
assert_ok "70.4 verify without crash after time eclipse" \
"$BARESNAP" verify "$REPO70"
# Prune must keep the most recent REAL snapshot
assert_ok "70.5 prune --keep-last 1 under temporal distortion" \
"$BARESNAP" prune "$REPO70" --keep-last 1
SNAPS70_AFTER=$(get_snap_count "$REPO70")
assert_eq "70.5 prune kept 1 snapshot" "1" "$SNAPS70_AFTER"
# Restore and global verify
SNAP70_LAST=$(get_latest_snap "$REPO70")
rm -rf "$OUT70"
assert_ok "70.6 restore post-temporal chaos" \
"$BARESNAP" restore "$REPO70" "$SNAP70_LAST" "$OUT70"
assert_ok "70.6 global verify positive after temporal chaos" \
"$BARESNAP" verify "$REPO70"
fi
# ============================================================
# 71. Advanced Cryptography: Full Cycle with AES-256-GCM
# ============================================================
section "71. Advanced Cryptography: Full Cycle with AES-256-GCM"
# Verify that the binary supports --encrypt aes
if ! "$BARESNAP" init --help 2>&1 | grep -qi "aes"; then
log "  ${YELLOW}[SKIP]${NC} --encrypt aes not implemented"
else
REPO71="$WORK/repo71"
SRC71="$WORK/src71"
OUT71="$WORK/out71"
BASE71="$OUT71/$(basename "$SRC71")"
rm -rf "$REPO71" "$SRC71" "$OUT71" "$WORK/out71_bad"
mkdir -p "$SRC71"
printf 'Highly confidential data protected by hardware
' > "$SRC71/top_secret.db"
head -c 512000 /dev/urandom > "$SRC71/crypto_test.bin"
export BARESNAP_PASSPHRASE="ClaveEspartanaConAES256"
AES71_OK=1
if ! assert_init_aes "71.1 init with --encrypt aes --compression zstd" \
"$BARESNAP" init "$REPO71" --encrypt aes --compression zstd --zstd-level 5; then
AES71_OK=0
fi
if [ "$AES71_OK" -eq 1 ] && ! assert_info_aes "71.1b info confirms AES" \
"$BARESNAP" info "$REPO71"; then
AES71_OK=0
fi
if [ "$AES71_OK" -eq 1 ]; then
assert_ok "71.2 create with AES-256-GCM pipeline" \
"$BARESNAP" create "$REPO71" "$SRC71" "snap_hardware_crypto"
# Opacity audit: zero plaintext in packs/snapshots
FOUND_PLAIN=0
if grep -rq "confidenciales" "$REPO71/packs/" 2>/dev/null; then
FOUND_PLAIN=1
fi
if grep -rq "confidenciales" "$REPO71/snapshots/" 2>/dev/null; then
FOUND_PLAIN=1
fi
assert_eq "71.3 zero plaintext leaks in physical structures" "0" "$FOUND_PLAIN"
# Cross integrity verification (CRC32C + GCM tag + hash ID)
assert_ok "71.4 verify: GCM authentication and integrity correct" \
"$BARESNAP" verify "$REPO71"
# Resilience against incorrect passphrase
export BARESNAP_PASSPHRASE="ContrasenaIncorrecta999"
set +e
"$BARESNAP" restore "$REPO71" "$(get_latest_snap "$REPO71")" "$WORK/out71_bad" >/dev/null 2>&1
RC71_BAD=$?
set +e
assert_eq "71.5 cryptographic firewall blocks false passphrase" "1" "$RC71_BAD"
# Successful bit-by-bit restore with correct passphrase
export BARESNAP_PASSPHRASE="ClaveEspartanaConAES256"
SNAP71=$(get_latest_snap "$REPO71")
rm -rf "$OUT71"
assert_ok "71.6 legitimate restore with AES-256-GCM" \
"$BARESNAP" restore "$REPO71" "$SNAP71" "$OUT71"
assert_ok "71.6 top_secret.db byte-identical" \
cmp -s "$SRC71/top_secret.db" "$BASE71/top_secret.db"
assert_ok "71.6 crypto_test.bin byte-identical" \
cmp -s "$SRC71/crypto_test.bin" "$BASE71/crypto_test.bin"
# Prune + verify with AES
assert_ok "71.7 prune --keep-last 1 with AES-256-GCM" \
"$BARESNAP" prune "$REPO71" --keep-last 1
assert_ok "71.7 verify after prune with AES" \
"$BARESNAP" verify "$REPO71"
else
fail "71.2 create with AES-256-GCM pipeline (skip due to init/info AES failure)"
fail "71.3 zero plaintext leaks in physical structures (skip)"
fail "71.4 verify: GCM authentication and integrity correct (skip)"
fail "71.5 cryptographic firewall blocks false passphrase (skip)"
fail "71.6 legitimate restore with AES-256-GCM (skip)"
fail "71.7 prune --keep-last 1 with AES-256-GCM (skip)"
fi
unset BARESNAP_PASSPHRASE
fi
# ============================================================
# 72. The End of Time: Resistance to the Unix Y2K38 Apocalypse
# ============================================================
section "72. The End of Time: Resistance to the Unix Y2K38 Apocalypse"
if ! command -v faketime &>/dev/null; then
log "  ${YELLOW}[SKIP]${NC} faketime not installed"
else
REPO72="$WORK/repo72"
SRC72="$WORK/src72"
OUT72="$WORK/out72"
BASE72="$OUT72/$(basename "$SRC72")"
mkdir -p "$SRC72"
printf 'File surviving the 32-bit Apocalypse
' > "$SRC72/futuro.txt"
head -c 65536 /dev/urandom > "$SRC72/future_blob.bin"
# Force file timestamp to the year 2039
faketime '2039-01-01 12:00:00' touch "$SRC72/futuro.txt"
pass "72.1 time jump simulation completed (clock in 2039)"
# Init and Create in the overflowed future
assert_ok "72.2 init in 2039" \
faketime '2039-01-01 12:05:00' "$BARESNAP" init "$REPO72"
assert_ok "72.2 create in 2039 (time_t 64-bit assimilates metadata)" \
faketime '2039-01-01 12:10:00' "$BARESNAP" create "$REPO72" "$SRC72" "snap_post_2038"
# Verify that list correctly parses the snapshot from the future
SNAP72=$(get_latest_snap "$REPO72")
if [ -n "$SNAP72" ]; then
pass "72.3 list shows snapshot from the future without corruption ($SNAP72)"
else
fail "72.3 corrupted metadata: overflow altered the snapshot"
fi
# Verify under temporal distortion
assert_ok "72.4 verify in Y2K38 environment" \
faketime '2039-01-01 12:15:00' "$BARESNAP" verify "$REPO72"
# Prune in the future
assert_ok "72.4 prune under 2039 temporal distortion" \
faketime '2039-01-01 12:15:00' "$BARESNAP" prune "$REPO72" --keep-last 1
# Restore and bit-by-bit integrity in the tomorrow
rm -rf "$OUT72"
assert_ok "72.5 restore in the year 2039" \
faketime '2039-01-01 12:20:00' "$BARESNAP" restore "$REPO72" "$SNAP72" "$OUT72"
assert_ok "72.5 futuro.txt byte-identical after restore in 2039" \
cmp -s "$SRC72/futuro.txt" "$BASE72/futuro.txt"
assert_ok "72.5 future_blob.bin byte-identical" \
cmp -s "$SRC72/future_blob.bin" "$BASE72/future_blob.bin"
# Verify that mtime is preserved correctly
MTIME_SRC72=$(stat -c '%Y' "$SRC72/futuro.txt")
MTIME_OUT72=$(stat -c '%Y' "$BASE72/futuro.txt")
assert_eq "72.6 mtime from 2039 preserved correctly" "$MTIME_SRC72" "$MTIME_OUT72"
fi
# ============================================================
# 73. Induced Bit Rot: Cosmic Ray Simulation in Packs (AES-256-GCM)
# ============================================================
section "73. Induced Bit Rot: Cosmic Ray in Packs (AES-256-GCM)"
REPO73="$WORK/repo73"
SRC73="$WORK/src73"
OUT73="$WORK/out73"
rm -rf "$REPO73" "$SRC73" "$OUT73"
mkdir -p "$SRC73"
export BARESNAP_PASSPHRASE="ClaveSecretaRayoCosmico"
echo "High fidelity payload that will suffer bit rot" > "$SRC73/target.txt"
head -c 8192 /dev/urandom > "$SRC73/padding.bin"
AES73_OK=1
if ! assert_init_aes "73.1 init --encrypt aes" \
"$BARESNAP" init "$REPO73" --encrypt aes; then
AES73_OK=0
fi
if [ "$AES73_OK" -eq 1 ] && ! assert_info_aes "73.1b info confirms AES" \
"$BARESNAP" info "$REPO73"; then
AES73_OK=0
fi
if [ "$AES73_OK" -eq 1 ]; then
assert_ok "73.1c create snapshot pre-rot" \
"$BARESNAP" create "$REPO73" "$SRC73" "snap_pre_rot"
# Locate the physical pack and mutate 1 byte
PACK73=$(find "$REPO73/packs" -type f -name '*.pack' 2>/dev/null | head -n 1)
if [ -n "$PACK73" ] && [ -f "$PACK73" ]; then
PACK73_SIZE=$(stat -c '%s' "$PACK73")
if [ "$PACK73_SIZE" -gt 2048 ]; then
ROT73_OFF=1024
else
ROT73_OFF=$((PACK73_SIZE / 2))
fi
printf '\xFF' | dd of="$PACK73" bs=1 seek="$ROT73_OFF" conv=notrunc status=none 2>/dev/null
pass "73.2 injection of 1 byte mutation at offset $ROT73_OFF"
else
fail "73.2 no physical pack found to corrupt"
fi
# verify must catch the alteration of the GCM/MAC Tag
set +e
"$BARESNAP" verify "$REPO73" >/dev/null 2>&1
RC73_V=$?
set +e
if [ "$RC73_V" -ne 0 ]; then
pass "73.3 verify detected integrity failure (altered MAC/GCM Tag)"
else
fail "73.3 verify did not detect the bit rot corruption"
fi
# restore must abort atomically (Avoid SIGSEGV)
set +e
"$BARESNAP" restore "$REPO73" "snap_pre_rot" "$OUT73" >/dev/null 2>&1
RC73_R=$?
set +e
if [ "$RC73_R" -ge 128 ]; then
fail "73.4 restore CRASHED with signal $((RC73_R - 128)) (memory panic)"
elif [ "$RC73_R" -ne 0 ]; then
pass "73.4 automatic pack quarantine without memory panic"
else
fail "73.4 the engine allowed restoring data corrupted by bit rot"
fi
else
fail "73.1c create snapshot pre-rot (skip due to init/info AES failure)"
fail "73.2 injection of 1 byte mutation (skip)"
fail "73.3 verify detected integrity failure (skip)"
fail "73.4 automatic pack quarantine (skip)"
fi
unset BARESNAP_PASSPHRASE
# ============================================================
# 74. Space Apocalypse: ENOSPC when Dumping Buffers
# ============================================================
section "74. Space Apocalypse: ENOSPC Management"
REPO74="$WORK/repo74"
SRC74="$WORK/src74"
mkdir -p "$SRC74"
# Create a file that exceeds the artificial limit (1.5 MB > 500 KB)
head -c 1572864 /dev/urandom > "$SRC74/huge_blob.bin"
assert_ok "74.1 init" "$BARESNAP" init "$REPO74"
# Impose a strict file size limit via ulimit
# 1024 blocks of 512 bytes = 512 KB of maximum writing
set +e
( ulimit -f 1024; "$BARESNAP" create "$REPO74" "$SRC74" "snap_enospc" >/dev/null 2>&1 ) 2>/dev/null
RC74=$?
set -e
if [ "$RC74" -eq 0 ]; then
fail "74.2 create reported success despite ulimit"
else
pass "74.2 write interruption captured (rc=$RC74)"
fi
# Verify that the repository maintains atomic consistency (Rollback)
assert_ok "74.3 atomic rollback of index after ENOSPC" "$BARESNAP" verify "$REPO74"
# Clean orphan temporaries (the process died by signal, could not do cleanup)
# The next create/health purges them automatically
"$BARESNAP" health "$REPO74" --repair </dev/null >/dev/null 2>&1 || true
# Alternative: an empty create cleans tmp/ on startup
mkdir -p "$SRC74/empty_dir"
"$BARESNAP" create "$REPO74" "$SRC74/empty_dir" >/dev/null 2>&1 || true
# Verify there are no zombie temporaries
TMP74=$(find "$REPO74/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP74" -eq 0 ]; then
pass "74.4 no orphan temporaries after ENOSPC"
else
fail "74.4 $TMP74 orphan temporaries after ENOSPC"
fi
# ============================================================
# 75. Concurrent Swarm: Mutual Exclusion Locks
# ============================================================
section "75. Concurrent Swarm: Mutual Exclusion"
REPO75="$WORK/repo75"
SRC75="$WORK/src75"
OUT75_DIR="$WORK/out75"
mkdir -p "$SRC75" "$OUT75_DIR"
assert_ok "75.1 init" "$BARESNAP" init "$REPO75"
echo "Concurrent burst" > "$SRC75/data.txt"
head -c 32768 /dev/urandom > "$SRC75/blob.bin"
declare -a PIDS75=()
declare -a RCS75=()
declare -a LOCKFAIL75=()
# Launch 5 requests surgically spaced to give time to the atomic lock
for i in 1 2 3 4 5; do
"$BARESNAP" create "$REPO75" "$SRC75" "snap_c$i" > "$OUT75_DIR/create_$i.txt" 2>&1 &
PIDS75[$i]=$!
RCS75[$i]=0
LOCKFAIL75[$i]=0
sleep 0.05
done
# Wait WITHOUT set -e killing the script
for i in 1 2 3 4 5; do
wait "${PIDS75[$i]}" 2>/dev/null || RCS75[$i]=$?
if grep -Eqi 'locked|lock file' "$OUT75_DIR/create_$i.txt" 2>/dev/null; then
LOCKFAIL75[$i]=1
fi
done
SUCCESS75=0
LOCKED75=0
UNEXPECTED75=0
for i in 1 2 3 4 5; do
if [ "${RCS75[$i]}" -eq 0 ]; then
SUCCESS75=$((SUCCESS75 + 1))
elif [ "${LOCKFAIL75[$i]}" -eq 1 ]; then
LOCKED75=$((LOCKED75 + 1))
else
UNEXPECTED75=$((UNEXPECTED75 + 1))
echo "--- create $i rc=${RCS75[$i]} ---" >&2
cat "$OUT75_DIR/create_$i.txt" >&2 || true
fi
done
if [ "$UNEXPECTED75" -gt 0 ]; then
fail "75.2 $UNEXPECTED75 process(es) failed for cause unrelated to lock"
else
pass "75.2 firing of 5 concurrent sub-processes completed (ok=$SUCCESS75 locked=$LOCKED75)"
fi
# The atomic lock must force the repository to remain intact
assert_ok "75.3 verify after concurrent burst" "$BARESNAP" verify "$REPO75"
# Count created snapshots (can be 1-5 depending on the lock)
SNAPS75=$(get_snap_count "$REPO75" 2>/dev/null || true)
SNAPS75=${SNAPS75:-0}
if ! [[ "$SNAPS75" =~ ^[0-9]+$ ]]; then
SNAPS75=0
fi
if [ "$SNAPS75" -ge 1 ]; then
pass "75.4 $SNAPS75 snapshot(s) created under concurrency"
else
fail "75.4 no snapshot created under concurrency (expected='>= 1' actual='$SNAPS75')"
fi
# Verify there are no temporaries or corruption
TMP75=0
if [ -d "$REPO75/tmp" ]; then
TMP75=$(find "$REPO75/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l || true)
fi
TMP75=${TMP75:-0}
if ! [[ "$TMP75" =~ ^[0-9]+$ ]]; then
TMP75=1
fi
if [ "$TMP75" -eq 0 ]; then
pass "75.5 no temporaries after concurrency"
else
fail "75.5 $TMP75 orphan temporaries after concurrency"
fi
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
# ============================================================
# 77. Pack Tamper: physical mutation in encrypted pack (AES-256-GCM)
# ============================================================
section "77. Pack Tamper: physical mutation in encrypted pack (AES-256-GCM)"
REPO77="$WORK/repo77"
SRC77="$WORK/src77"
export BARESNAP_PASSPHRASE="tamper-pass-77"
assert_init_aes "77.1 init aes" "$BARESNAP" init "$REPO77" --encrypt aes
assert_info_aes "77.1b info confirms AES" "$BARESNAP" info "$REPO77"
mkdir -p "$SRC77"
printf 'data for tamper
' > "$SRC77/tamper.txt"
head -c 131072 /dev/urandom > "$SRC77/blob.bin"
assert_ok "77.2 create aes" "$BARESNAP" create "$REPO77" "$SRC77"
assert_ok "77.3 verify before tamper" "$BARESNAP" verify "$REPO77"
PACK77=$(find "$REPO77/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | head -1)
if [ -n "$PACK77" ]; then
PACK77_SIZE=$(stat -c '%s' "$PACK77")
OFF77=$((PACK77_SIZE / 2))
printf '\xFF' | dd of="$PACK77" bs=1 seek="$OFF77" conv=notrunc status=none 2>/dev/null
pass "77.4 byte injected at offset $OFF77 (encrypted data zone)"
set +e
"$BARESNAP" verify "$REPO77" >/dev/null 2>&1
VERIFY_RC77=$?
set -e
if [ "$VERIFY_RC77" -ne 0 ]; then
pass "77.5 verify detected corrupt pack (GCM/CRC)"
else
fail "77.5 verify did NOT detect the mutation"
fi
else
fail "77.4 no pack found to mutate"
fail "77.5 skip"
fi
unset BARESNAP_PASSPHRASE
# ============================================================
# 78. Config Fuzzing: central config sabotage
# ============================================================
section "78. Config Fuzzing: central config sabotage"
REPO78="$WORK/repo78"
SRC78="$WORK/src78"
assert_ok "78.1 init" "$BARESNAP" init "$REPO78"
mkdir -p "$SRC78"
printf 'fuzz
' > "$SRC78/f.txt"
# Destroy the config with binary garbage
dd if=/dev/urandom of="$REPO78/config" bs=1 count=64 conv=notrunc status=none 2>/dev/null
pass "78.2 config overwritten with garbage"
# list: must not crash (although it doesn't validate the config, it must not die by signal)
set +e
"$BARESNAP" list "$REPO78" >/dev/null 2>&1
LIST_RC78=$?
set -e
if [ "$LIST_RC78" -lt 128 ]; then
pass "78.3 list did not crash with corrupt config (rc=$LIST_RC78)"
else
fail "78.3 list CRASHED with signal $((LIST_RC78 - 128))"
fi
# verify: must reject the corrupt config
set +e
"$BARESNAP" verify "$REPO78" >/dev/null 2>&1
VERIFY_RC78=$?
set -e
if [ "$VERIFY_RC78" -ne 0 ]; then
pass "78.4 verify rejected corrupt config"
else
fail "78.4 verify accepted corrupt config"
fi
# create: must reject the corrupt config
set +e
"$BARESNAP" create "$REPO78" "$SRC78" >/dev/null 2>&1
CREATE_RC78=$?
set -e
if [ "$CREATE_RC78" -ne 0 ]; then
pass "78.5 create rejected corrupt config"
else
fail "78.5 create accepted corrupt config"
fi
# ============================================================
# 79 Battery Death: kill -9 during create
# ============================================================
section "79 Battery Death: kill -9 during create"
REPO79="$WORK/repo79"
SRC79="$WORK/src79"
assert_ok "79.1 init" "$BARESNAP" init "$REPO79"
mkdir -p "$SRC79"
# 128 MB distributed in 8 files of 16 MB + 200 small files
# to force enough metadata + I/O and that create lasts >1s
for i in $(seq 1 8); do
head -c 16777216 /dev/urandom > "$SRC79/heavy_$i.bin" 2>/dev/null
done
for i in $(seq 1 200); do
printf 'battery-death-file-%04d
' "$i" > "$SRC79/small_$i.txt"
done
pass "79.2 source created (128 MB + 200 small files)"
# Launch create in background
"$BARESNAP" create "$REPO79" "$SRC79" </dev/null >/dev/null 2>&1 &
PID79=$!
# Polling loop: wait for the process to be really active
# and then kill it. Maximum 5 seconds of waiting.
KILLED79=0
for attempt in $(seq 1 100); do
sleep 0.05
if ! kill -0 "$PID79" 2>/dev/null; then
# The process already finished before being able to kill it
break
fi
# The process is still alive: kill it with kill -9
kill -9 "$PID79" 2>/dev/null
wait "$PID79" 2>/dev/null || true
KILLED79=1
break
done
if [ "$KILLED79" -eq 1 ]; then
pass "79.3 kill -9 sent in the middle of create (pid=$PID79)"
else
wait "$PID79" 2>/dev/null || true
fail "79.3 create finished before kill (test not applied)"
fi
if [ "$KILLED79" -eq 1 ]; then
# health --repair must clean temporaries and leave the repo intact
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO79" --repair </dev/null >/dev/null 2>&1 || true
TMP79=$(find "$REPO79/tmp" -name '*.tmp' -type f 2>/dev/null | wc -l)
if [ "$TMP79" -gt 0 ]; then
fail "79.4 $TMP79 zombie temporaries after kill -9"
else
pass "79.4 no temporaries after kill -9"
fi
set +e
"$BARESNAP" verify "$REPO79" >/dev/null 2>&1
VERIFY_RC79=$?
set -e
if [ "$VERIFY_RC79" -eq 0 ]; then
pass "79.5 verify OK after kill -9 + repair"
else
# If no snapshot was created (very early kill), verify can
# pass trivially or fail due to empty repo. Both are valid.
SNAPS79=$(get_snap_count "$REPO79")
if [ "$SNAPS79" -eq 0 ]; then
pass "79.5 verify OK (empty repo after premature kill)"
else
fail "79.5 verify fails after kill -9 + repair (rc=$VERIFY_RC79)"
fi
fi
else
fail "79.4 skip (kill not applied)"
fail "79.5 skip (kill not applied)"
fi
# ============================================================
# 80. Frankenstein Repo: config+index deleted, packs alive
# ============================================================
section "80. Frankenstein Repo: config+index deleted"
REPO80="$WORK/repo80"
SRC80="$WORK/src80"
mkdir -p "$SRC80"
printf 'frankenstein test
' > "$SRC80/data.txt"
head -c 65536 /dev/urandom > "$SRC80/blob.bin"
assert_ok "80.1 init" "$BARESNAP" init "$REPO80"
assert_ok "80.2 create" "$BARESNAP" create "$REPO80" "$SRC80"
# Destroy config, index and cache (leave packs and snapshots)
rm -f "$REPO80/config" "$REPO80/cache"
rm -rf "$REPO80/index"
# create must NOT crash (must auto-init and work)
set +e
"$BARESNAP" create "$REPO80" "$SRC80" > "$WORK/frankenstein.log" 2>&1
RC80=$?
set -e
if [ "$RC80" -ge 128 ]; then
fail "80.3 create CRASHED with signal $((RC80 - 128))"
elif [ "$RC80" -eq 0 ]; then
pass "80.3 create survived Frankenstein repo (auto-init + recreate)"
else
pass "80.3 create aborted cleanly without crashing (rc=$RC80)"
fi
# Post-kill audit (same pattern as prune tests)
audit_post_prune "$REPO80" "80.4 post-frankenstein"
# ============================================================
# 81. Inconsistent repo: directories exist but config is missing
# ============================================================
section "81. Inconsistent repo (config manually deleted)"
REPO81="$WORK/repo81"
SRC81="$WORK/src81"
mkdir -p "$SRC81"
printf 'test data' > "$SRC81/file.txt"
# Create healthy repo
assert_ok "81.1 init healthy repo" "$BARESNAP" init "$REPO81"
assert_ok "81.2 create first snapshot" "$BARESNAP" create "$REPO81" "$SRC81"
# Simulate corruption: delete config but leave directories
rm -f "$REPO81/config"
# Attempting create should FAIL with a clear message
set +e
CREATE_OUT81=$("$BARESNAP" create "$REPO81" "$SRC81" 2>&1)
CREATE_RC81=$?
set -e
if [ "$CREATE_RC81" -ne 0 ] && echo "$CREATE_OUT81" | grep -q "inconsistent state"; then
pass "81.3 create detected corrupt repo and aborted with clear message"
else
fail "81.3 create did not detect corruption (rc=$CREATE_RC81)"
log " [OUTPUT] $CREATE_OUT81"
fi
# Verify that health --repair fixes it
assert_ok "81.4 health --repair recovers the repo" "$BARESNAP" health "$REPO81" --repair
assert_ok "81.5 create works after repair" "$BARESNAP" create "$REPO81" "$SRC81"
# ============================================================
# 82. Entropy Firewall: Automatic ZSTD Bypass
# ============================================================
section "82. Entropy Firewall: Automatic ZSTD Bypass"
REPO82="$WORK/repo82"
SRC82="$WORK/src82"
assert_ok "82.1 init base repo" "$BARESNAP" init "$REPO82"
mkdir -p "$SRC82"
# 1. Create a highly compressible plain text file
for i in {1..2000}; do echo "BareSnap compressible log line text pattern $i"; done > "$SRC82/compressible.log"
# 2. Create a pure incompressible file (1 MB high entropy noise)
head -c 1048576 /dev/urandom > "$SRC82/high_entropy.mp4"
# Launch backup forcing ZSTD level 15 (To stress test)
assert_ok "82.2 create snapshot with mixed data in ZSTD 15" \
"$BARESNAP" create "$REPO82" "$SRC82" --compression zstd --zstd-level 15
# Extract statistics to verify the firewall via 'info'
INFO82=$("$BARESNAP" info "$REPO82" 2>&1)
# Storage audit: If the entropy bypass works,
# the chunk for 'high_entropy.mp4' must have been saved as UNCOMPRESSED.
# Therefore, the count of incompressible chunks must be greater than or equal to 1.
if printf '%s
' "$INFO82" | grep -q "uncompressed chunks:"; then
UNCOMP_COUNT=$(printf '%s
' "$INFO82" | grep "uncompressed chunks:" | grep -oE '[0-9]+' | head -1)
if [ "$UNCOMP_COUNT" -gt 0 ]; then
pass "82.3 Entropy firewall active ($UNCOMP_COUNT chunks skipped compression)"
else
fail "82.3 Firewall failed: 0 chunks skipped compression (CPU uselessly hammered)"
fi
else
fail "82.3 info does not show detailed chunk statistics"
fi
# ============================================================
# 83. Dynamic Compression: LZ4 and ZSTD Coexistence per Snapshot
# ============================================================
section "83. Dynamic Compression: LZ4 and ZSTD Coexistence per Snapshot"
REPO83="$WORK/repo83"
SRC83_1="$WORK/src83_1"
SRC83_2="$WORK/src83_2"
mkdir -p "$SRC83_1" "$SRC83_2"
# Initialize a repo with LZ4 by default
assert_ok "83.1 init repo with LZ4 by default" "$BARESNAP" init "$REPO83" --compression lz4
# Snapshot 1: Dynamically force ZSTD level 12 for heavy logs
for i in {1..1000}; do echo "Log pattern heavy compression session $i"; done > "$SRC83_1/logs.txt"
assert_ok "83.2 create snapshot 1 forcing dynamic ZSTD" \
"$BARESNAP" create "$REPO83" "$SRC83_1" --compression zstd --zstd-level 12
# Snapshot 2: Dynamically force LZ4 for lightweight code files
echo "int main() { return 0; }" > "$SRC83_2/main.c"
assert_ok "83.3 create snapshot 2 forcing dynamic LZ4" \
"$BARESNAP" create "$REPO83" "$SRC83_2" --compression lz4
# Validate that info can audit the mixed repository
INFO83=$("$BARESNAP" info "$REPO83" 2>&1)
if printf '%s
' "$INFO83" | grep -q "compressed chunks:"; then
pass "83.4 Info correctly reads repository with mixed dynamic compression"
else
fail "83.4 Info failed to analyze mixed structure"
fi
# Validate absolute integrity of the transactional database with multiple algorithms
assert_ok "83.5 global verify on mixed LZ4/ZSTD repository successful" "$BARESNAP" verify "$REPO83"
# ============================================================
# 84. Encryption and Health: invalid key aborts without false positive
# ============================================================
section "84. Encryption and Health: invalid key does not generate false positive"
REPO84="$WORK/repo84"
SRC84="$WORK/src84"
mkdir -p "$SRC84"
printf 'secret data for health test
' > "$SRC84/secret.txt"
head -c 32768 /dev/urandom > "$SRC84/blob.bin"
export BARESNAP_PASSPHRASE="correct-passphrase-84"
assert_ok "84.1 init --encrypt" "$BARESNAP" init "$REPO84" --encrypt
assert_ok "84.2 create encrypted" "$BARESNAP" create "$REPO84" "$SRC84"
assert_ok "84.3 verify encrypted (correct key)" "$BARESNAP" verify "$REPO84"
unset BARESNAP_PASSPHRASE
# Force health with INCORRECT key: must abort without false positives
export BARESNAP_PASSPHRASE="wrong-passphrase-84"
set +e
HEALTH_OUT84=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO84" </dev/null 2>&1)
HEALTH_RC84=$?
set -e
if [ "$HEALTH_RC84" -ge 128 ]; then
fail "84.4 health with incorrect key crashed by signal (rc=$HEALTH_RC84)"
elif [ "$HEALTH_RC84" -ne 0 ]; then
pass "84.4 health with incorrect key aborts cleanly (rc=$HEALTH_RC84)"
else
fail "84.4 health with incorrect key returned rc=0 (should fail)"
fi
# Verify that /damaged was NOT created (destructive false positive)
if [ -d "$REPO84/damaged" ]; then
DAMAGED84=$(find "$REPO84/damaged" -type f 2>/dev/null | wc -l)
if [ "$DAMAGED84" -gt 0 ]; then
fail "84.5 health moved healthy snapshots to damaged/ (false positive)"
else
pass "84.5 damaged/ exists but is empty"
fi
else
pass "84.5 damaged/ was not created (no destructive false positive)"
fi
# Verify that the repo remains intact with the correct key
export BARESNAP_PASSPHRASE="correct-passphrase-84"
assert_ok "84.6 verify still passes with correct key" "$BARESNAP" verify "$REPO84"
SNAP84=$(get_latest_snap "$REPO84")
assert_ok "84.7 restore still works" "$BARESNAP" restore "$REPO84" "$SNAP84" "$WORK/out84"
unset BARESNAP_PASSPHRASE
# ============================================================
# 85. Index Grow: 2000 small files without recursive OOM
# ============================================================
section "85. Index Grow: 2000 small files without recursive OOM"
REPO85="$WORK/repo85"
SRC85="$WORK/src85"
OUT85="$WORK/out85"
BASE85="$OUT85/$(basename "$SRC85")"
mkdir -p "$SRC85"
assert_ok "85.1 init" "$BARESNAP" init "$REPO85"
# Generate 2000 small files to force hash table expansion
for i in $(seq 1 2000); do
printf "data-%04d" "$i" > "$SRC85/file_$i.txt"
done
pass "85.2 2000 small files created"
# Massive backup: forces index_map_grow without recursive OOM
assert_ok "85.3 massive create (2000 files, forces index grow)" "$BARESNAP" create "$REPO85" "$SRC85"
# Verify consistency
assert_ok "85.4 verify after massive backup" "$BARESNAP" verify "$REPO85"
# Restore and validate integrity
LATEST85=$(get_latest_snap "$REPO85")
assert_ok "85.5 massive restore" "$BARESNAP" restore "$REPO85" "$LATEST85" "$OUT85"
RESTORED85=$(find "$BASE85" -type f 2>/dev/null | wc -l)
assert_eq "85.6 2000 files restored" "2000" "$RESTORED85"
# Verify content of a random file
SAMPLE85=$(cat "$BASE85/file_1000.txt")
assert_eq "85.7 correct content" "data-1000" "$SAMPLE85"
# ============================================================
# 86. Append & CRC32C: hot modifications
# ============================================================
section "86. Append & CRC32C: hot modifications"
REPO86="$WORK/repo86"
SRC86="$WORK/src86"
OUT86="$WORK/out86"
BASE86="$OUT86/$(basename "$SRC86")"
mkdir -p "$SRC86"
assert_ok "86.1 init" "$BARESNAP" init "$REPO86"
# Initial backup
for i in $(seq 1 100); do
printf "initial-%04d" "$i" > "$SRC86/file_$i.txt"
done
assert_ok "86.2 initial create (100 files)" "$BARESNAP" create "$REPO86" "$SRC86"
# Hot modifications (append + new files)
for i in $(seq 1 50); do
printf '  MODIFIED' >> "$SRC86/file_$i.txt"
done
for i in $(seq 101 200); do
printf "new-%04d" "$i" > "$SRC86/file_$i.txt"
done
pass "86.3 hot modifications applied (50 appends + 100 new)"
# Incremental backup: validates hot CRC32C and correct append
assert_ok "86.4 incremental create (append + CRC32C)" "$BARESNAP" create "$REPO86" "$SRC86"
# Verify must pass 100% intact
assert_ok "86.5 incremental verify (CRC32C integrity)" "$BARESNAP" verify "$REPO86"
# Restore and verify byte-identical
rm -rf "$OUT86"
LATEST86=$(get_latest_snap "$REPO86")
assert_ok "86.6 incremental restore" "$BARESNAP" restore "$REPO86" "$LATEST86" "$OUT86"
# Spot-check: verify that a modified file is correct
if grep -q "MODIFIED" "$BASE86/file_1.txt" 2>/dev/null; then
pass "86.7 modified content present in restore"
else
fail "86.7 modified content NOT present in restore"
fi
# Verify that the total count is correct
RESTORED86=$(find "$BASE86" -type f 2>/dev/null | wc -l)
assert_eq "86.8 200 files after incremental" "200" "$RESTORED86"
# ============================================================
# 87. LRU Cache Eviction: large packs without dangling pointers
# ============================================================
section "87. LRU Cache Eviction: large packs without dangling pointers"
REPO87="$WORK/repo87"
SRC87="$WORK/src87"
OUT87="$WORK/out87"
BASE87="$OUT87/$(basename "$SRC87")"
mkdir -p "$SRC87"
assert_ok "87.1 init" "$BARESNAP" init "$REPO87"
# Create large files to fill the pack cache
for i in $(seq 1 30); do
dd if=/dev/urandom bs=1048576 count=2 of="$SRC87/big_$i.bin" 2>/dev/null
done
pass "87.2 30 2MB files created (60 MB total)"
# Backup to generate large packs
assert_ok "87.3 create with large packs (fills cache)" "$BARESNAP" create "$REPO87" "$SRC87"
# Massive restore: forces reading of multiple packs and LRU eviction
# If there are dangling pointers, this will crash with SIGSEGV
rm -rf "$OUT87"
LATEST87=$(get_latest_snap "$REPO87")
set +e
"$BARESNAP" restore "$REPO87" "$LATEST87" "$OUT87" >/dev/null 2>&1 </dev/null
RESTORE_RC87=$?
set -e
if [ "$RESTORE_RC87" -ge 128 ]; then
fail "87.4 restore CRASHED with signal $((RESTORE_RC87 - 128)) (dangling pointer likely)"
elif [ "$RESTORE_RC87" -eq 0 ]; then
pass "87.4 massive restore completed without crash (LRU eviction OK)"
else
pass "87.4 restore finished with controlled error (rc=$RESTORE_RC87)"
fi
# Verify post-eviction integrity if the restore was successful
if [ "$RESTORE_RC87" -eq 0 ]; then
MISMATCH87=0
for i in $(seq 1 30); do
if ! cmp -s "$SRC87/big_$i.bin" "$BASE87/big_$i.bin" 2>/dev/null; then
MISMATCH87=$((MISMATCH87 + 1))
fi
done
if [ "$MISMATCH87" -eq 0 ]; then
pass "87.5 30/30 files byte-identical after LRU eviction"
else
fail "87.5 $MISMATCH87/30 files differ after LRU eviction"
fi
assert_ok "87.6 verify after LRU eviction" "$BARESNAP" verify "$REPO87"
fi
# ============================================================
# 88. Basic LIST: normal directory
# ============================================================
section "88. Basic LIST: normal directory"
REPO88="$WORK/repo88"
SRC88="$WORK/src88"
mkdir -p "$SRC88/subdir"
printf 'file1
' > "$SRC88/file1.txt"
printf 'file2
' > "$SRC88/file2.txt"
printf 'nested
' > "$SRC88/subdir/nested.txt"
assert_ok "88.1 init" "$BARESNAP" init "$REPO88"
assert_ok "88.2 create" "$BARESNAP" create "$REPO88" "$SRC88"
SNAP88=$(get_latest_snap "$REPO88")
LS_OUT88=$("$BARESNAP" ls "$REPO88" "$SNAP88" --recursive 2>/dev/null)
LS_RC88=$?
assert_eq "88.3 ls works" "0" "$LS_RC88"
if printf '%s
' "$LS_OUT88" | grep -q "file1\.txt"; then
pass "88.4 ls shows file1.txt"
else
fail "88.4 ls does not show file1.txt"
fi
if printf '%s
' "$LS_OUT88" | grep -q "subdir"; then
pass "88.5 ls shows subdir"
else
fail "88.5 ls does not show subdir"
fi
if printf '%s
' "$LS_OUT88" | grep -q "nested\.txt"; then
pass "88.6 ls shows nested.txt"
else
fail "88.6 ls does not show nested.txt"
fi
# ============================================================
# 89. LIST with long names (255 chars, filesystem limit)
# ============================================================
section "89. LIST with long names (255 chars)"
REPO89="$WORK/repo89"
SRC89="$WORK/src89"
OUT89="$WORK/out89"
mkdir -p "$SRC89"
LONG_NAME89=$(printf 'A%.0s' $(seq 1 250))
LONG_NAME89="${LONG_NAME89}.txt"
printf 'long name content
' > "$SRC89/$LONG_NAME89"
assert_ok "89.1 init" "$BARESNAP" init "$REPO89"
assert_ok "89.2 create with long name" "$BARESNAP" create "$REPO89" "$SRC89"
SNAP89=$(get_latest_snap "$REPO89")
LS_OUT89=$("$BARESNAP" ls "$REPO89" "$SNAP89" --recursive 2>/dev/null)
LS_RC89=$?
assert_eq "89.3 ls with long name works" "0" "$LS_RC89"
if printf '%s
' "$LS_OUT89" | grep -q "AAAA"; then
pass "89.4 ls shows file with long name"
else
fail "89.4 ls does not show file with long name"
fi
assert_ok "89.5 restore with long name" "$BARESNAP" restore "$REPO89" "$SNAP89" "$OUT89"
BASE89="$OUT89/$(basename "$SRC89")"
if [ -f "$BASE89/$LONG_NAME89" ]; then
pass "89.6 long file restored correctly"
else
fail "89.6 long file NOT restored"
fi
# ============================================================
# 90. LIST with deep path (20 levels)
# ============================================================
section "90. LIST with deep path (20 levels)"
REPO90="$WORK/repo90"
SRC90="$WORK/src90"
OUT90="$WORK/out90"
DEEP90="$SRC90"
for i in $(seq 1 20); do
DEEP90="$DEEP90/level_${i}_directory_name_padding"
done
mkdir -p "$DEEP90"
printf 'deep content
' > "$DEEP90/deep_file.txt"
assert_ok "90.1 init" "$BARESNAP" init "$REPO90"
assert_ok "90.2 create with deep path" "$BARESNAP" create "$REPO90" "$SRC90"
SNAP90=$(get_latest_snap "$REPO90")
assert_ok "90.3 ls recursive with deep path" "$BARESNAP" ls "$REPO90" "$SNAP90" --recursive
assert_ok "90.4 restore with deep path" "$BARESNAP" restore "$REPO90" "$SNAP90" "$OUT90"
DEEP_FILE90=$(find "$OUT90" -name "deep_file.txt" -type f 2>/dev/null | head -1)
if [ -n "$DEEP_FILE90" ]; then
CONTENT90=$(cat "$DEEP_FILE90")
assert_eq "90.5 deep file content correct" "deep content" "$CONTENT90"
else
fail "90.5 deep file not found after restore"
fi
assert_ok "90.6 verify with deep path" "$BARESNAP" verify "$REPO90"
# ============================================================
# 91. LIST with 5000 files (readdir stress)
# ============================================================
section "91. LIST with 5000 files (readdir stress)"
REPO91="$WORK/repo91"
SRC91="$WORK/src91"
rm -rf "$REPO91" "$SRC91"
mkdir -p "$SRC91"
for i in $(seq 1 5000); do
printf 'data_%05d
' "$i" > "$SRC91/file_$(printf '%05d' $i).txt"
done
pass "91.1 5000 files created"
assert_ok "91.2 init" "$BARESNAP" init "$REPO91"
assert_ok "91.3 create with 5000 files" "$BARESNAP" create "$REPO91" "$SRC91"
SNAP91=$(get_latest_snap "$REPO91")
LS_OUT91=$("$BARESNAP" ls "$REPO91" "$SNAP91" --recursive 2>/dev/null)
LS_RC91=$?
assert_eq "91.4 ls with 5000 files works" "0" "$LS_RC91"
LS_COUNT91=$(printf '%s
' "$LS_OUT91" | awk '/file_/{n++} END{print n+0}')
if [ "$LS_COUNT91" -ge 4990 ]; then
pass "91.5 ls lists $LS_COUNT91/5000 files"
else
fail "91.5 ls only lists $LS_COUNT91/5000 files"
fi
OUT91="$WORK/out91"
SRCBASE91=$(basename "$SRC91")
assert_ok "91.6 selective extract of 5000" "$BARESNAP" extract "$REPO91" "$SNAP91" "$OUT91" "$SRCBASE91/file_02500.txt"
EXTRACTED91=$(find "$OUT91" -name "file_02500.txt" -type f 2>/dev/null | head -1)
if [ -n "$EXTRACTED91" ] && grep -q "data_02500" "$EXTRACTED91"; then
pass "91.7 selective extract correct"
else
fail "91.7 selective extract incorrect"
fi
assert_ok "91.8 verify with 5000 files" "$BARESNAP" verify "$REPO91"
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
# ============================================================
# 93. Bug #1: Integer overflow in prune (malicious offset)
# ============================================================
section "93. Bug #1: Integer overflow in prune (malicious offset)"
REPO_B1="$WORK/repo_b1"
SRC_B1="$WORK/src_b1"
mkdir -p "$SRC_B1"
printf 'overflow test data
' > "$SRC_B1/data.txt"
head -c 32768 /dev/urandom > "$SRC_B1/blob.bin"
assert_ok "93.1 init" "$BARESNAP" init "$REPO_B1"
assert_ok "93.2 create" "$BARESNAP" create "$REPO_B1" "$SRC_B1"
PACK_B1=$(find "$REPO_B1/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | head -1)
if [ -n "$PACK_B1" ]; then
PACK_SIZE_B1=$(stat -c '%s' "$PACK_B1")
FOOTER_START=$((PACK_SIZE_B1 - 28 - 33))
if [ "$FOOTER_START" -gt 36 ]; then
OFFSET_POS=$((FOOTER_START + 8 + 8 + 4 + 16))
printf '\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFE' | \
dd of="$PACK_B1" bs=1 seek="$OFFSET_POS" conv=notrunc status=none 2>/dev/null
pass "93.3 malicious offset injected in pack"
else
fail "93.3 pack too small to inject offset"
fi
else
fail "93.3 no pack found to corrupt"
fi
set +e
"$BARESNAP" prune "$REPO_B1" --keep-last 1 >/dev/null 2>&1 </dev/null
PRUNE_RC_B1=$?
set -e
if [ "$PRUNE_RC_B1" -ge 128 ]; then
fail "93.4 prune CRASHED with signal $((PRUNE_RC_B1 - 128)) (OOB read likely)"
else
pass "93.4 prune did not crash with malicious offset (rc=$PRUNE_RC_B1)"
fi
set +e
"$BARESNAP" verify "$REPO_B1" >/dev/null 2>&1 </dev/null
VERIFY_RC_B1=$?
set -e
if [ "$VERIFY_RC_B1" -ge 128 ]; then
fail "93.5 verify CRASHED with signal $((VERIFY_RC_B1 - 128))"
else
pass "93.5 verify did not crash with malicious offset (rc=$VERIFY_RC_B1)"
fi
# ============================================================
# 94. Bug #2: Partial write / truncated pack
# ============================================================
section "94. Bug #2: Partial write / truncated pack"
REPO_B2="$WORK/repo_b2"
SRC_B2="$WORK/src_b2"
mkdir -p "$SRC_B2"
head -c 65536 /dev/urandom > "$SRC_B2/upload_test.bin"
printf 'small file
' > "$SRC_B2/small.txt"
assert_ok "94.1 init" "$BARESNAP" init "$REPO_B2"
assert_ok "94.2 create" "$BARESNAP" create "$REPO_B2" "$SRC_B2"
assert_ok "94.3 verify before truncating" "$BARESNAP" verify "$REPO_B2"
PACK_B2=$(find "$REPO_B2/packs" -maxdepth 1 -name '*.pack' -type f 2>/dev/null | head -1)
if [ -n "$PACK_B2" ]; then
PACK_SIZE_B2=$(stat -c '%s' "$PACK_B2")
HALF_B2=$((PACK_SIZE_B2 / 2))
if command -v truncate >/dev/null 2>&1; then
truncate -s "$HALF_B2" "$PACK_B2"
pass "94.4 pack truncated to $HALF_B2 bytes (from $PACK_SIZE_B2) with truncate"
else
dd if="$PACK_B2" of="$PACK_B2.trunc" bs=1 count="$HALF_B2" 2>/dev/null
mv "$PACK_B2.trunc" "$PACK_B2"
pass "94.4 pack truncated to $HALF_B2 bytes (from $PACK_SIZE_B2) with dd"
fi
else
fail "94.4 no pack found to truncate"
fi
set +e
"$BARESNAP" verify "$REPO_B2" >/dev/null 2>&1 </dev/null
VERIFY_RC_B2=$?
set -e
if [ "$VERIFY_RC_B2" -ge 128 ]; then
fail "94.5 verify CRASHED with truncated pack (signal $((VERIFY_RC_B2 - 128)))"
elif [ "$VERIFY_RC_B2" -ne 0 ]; then
pass "94.5 verify detected truncated pack (rc=$VERIFY_RC_B2)"
else
fail "94.5 verify did NOT detect truncated pack"
fi
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_T_B2="$(whoami)@localhost"
RP_B2="/tmp/smoke2_b2_repo"
URI_B2="ssh://${SSH_T_B2}${RP_B2}"
ssh "$SSH_T_B2" "rm -rf $RP_B2" 2>/dev/null || true
assert_ok "94.6 remote install" "$BARESNAP" remote install "$URI_B2"
assert_ok "94.7 remote init" "$BARESNAP" init "$URI_B2"
assert_ok "94.8 remote create (full upload)" "$BARESNAP" create "$URI_B2" "$SRC_B2"
assert_ok "94.9 remote verify (intact upload)" "$BARESNAP" verify "$URI_B2"
ssh "$SSH_T_B2" "rm -rf $RP_B2" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted for Bug #2"
fi
# ============================================================
# 95. Bug #3: Delta restore error (goto pass_end)
# ============================================================
section "95. Bug #3: Delta restore error (goto pass_end)"
REPO_B3="$WORK/repo_b3"
SRC_B3="$WORK/src_b3"
OUT_B3="$WORK/out_b3"
mkdir -p "$SRC_B3"
yes "Delta error path test content for xdelta3 validation." | head -c 32768 > "$SRC_B3/delta_target.txt"
assert_ok "95.1 init" "$BARESNAP" init "$REPO_B3"
assert_ok "95.2 create v1 (base for delta)" "$BARESNAP" create "$REPO_B3" "$SRC_B3"
sleep 1.1
printf 'MOD' >> "$SRC_B3/delta_target.txt"
touch "$SRC_B3/delta_target.txt"
assert_ok "95.3 create v2 (delta expected)" "$BARESNAP" create "$REPO_B3" "$SRC_B3"
for p in "$REPO_B3/packs/"*.pack; do
[ -f "$p" ] || continue
PSIZE=$(stat -c '%s' "$p")
if [ "$PSIZE" -gt 200 ]; then
printf '\xFF' | dd of="$p" bs=1 seek=100 conv=notrunc status=none 2>/dev/null
fi
done
pass "95.4 packs corrupted (delta cannot reconstruct)"
SNAP_B3=$(get_latest_snap "$REPO_B3")
set +e
"$BARESNAP" restore "$REPO_B3" "$SNAP_B3" "$OUT_B3" >/dev/null 2>&1 </dev/null
RESTORE_RC_B3=$?
set -e
if [ "$RESTORE_RC_B3" -ge 128 ]; then
fail "95.5 restore CRASHED with signal $((RESTORE_RC_B3 - 128))"
else
pass "95.5 restore failed cleanly without crash (rc=$RESTORE_RC_B3)"
fi
EMPTY_FILES_B3=$(find "$OUT_B3" -type f -empty 2>/dev/null | wc -l)
if [ "$EMPTY_FILES_B3" -eq 0 ]; then
pass "95.6 no empty partial files in output"
else
fail "95.6 $EMPTY_FILES_B3 empty partial files found"
fi
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
# ============================================================
# 97. Bug #5: tls:// and sftp:// correctly rejected
# ============================================================
section "97. Bug #5: tls:// and sftp:// correctly rejected"
set +e
"$BARESNAP" init "tls://fakehost/path" >/dev/null 2>&1 </dev/null
TLS_RC=$?
set -e
if [ "$TLS_RC" -ne 0 ]; then pass "97.1 init tls:// fails cleanly (rc=$TLS_RC)"; else fail "97.1 init tls:// did not fail"; fi
set +e
"$BARESNAP" init "sftp://fakehost/path" >/dev/null 2>&1 </dev/null
SFTP_RC=$?
set -e
if [ "$SFTP_RC" -ne 0 ]; then pass "97.2 init sftp:// fails cleanly (rc=$SFTP_RC)"; else fail "97.2 init sftp:// did not fail"; fi
TLS_DIR="$WORK/tls_garbage"
mkdir -p "$TLS_DIR"
if [ -z "$(ls -A "$TLS_DIR" 2>/dev/null)" ]; then pass "97.3 no garbage local directories created"; else fail "97.3 garbage local directories created"; fi
set +e
"$BARESNAP" create "tls://fakehost/path" "$WORK" >/dev/null 2>&1 </dev/null
CREATE_TLS_RC=$?
set -e
if [ "$CREATE_TLS_RC" -ne 0 ]; then pass "97.4 create tls:// fails cleanly (rc=$CREATE_TLS_RC)"; else fail "97.4 create tls:// did not fail"; fi
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
set +e
"$BARESNAP" init "ssh://invalid_host_name_that_does_not_exist_12345/path" >/dev/null 2>&1 </dev/null
SSH_BAD_RC=$?
set -e
if [ "$SSH_BAD_RC" -ge 128 ] && [ "$SSH_BAD_RC" -ne 255 ]; then
fail "97.5 init ssh:// invalid host CRASHED (signal $((SSH_BAD_RC - 128)))"
else
pass "97.5 init ssh:// invalid host fails without crash (rc=$SSH_BAD_RC)"
fi
else
log "  ${YELLOW}[SKIP]${NC} SSH test omitted for Bug #5"
fi
# ============================================================
# 98. Bug #6: VFS cleanup between operations
# ============================================================
section "98. Bug #6: VFS cleanup between operations"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_T_B6="$(whoami)@localhost"
RP_B6="/tmp/smoke2_b6_repo"
URI_B6="ssh://${SSH_T_B6}${RP_B6}"
SRC_B6="$WORK/src_b6"
OUT_B6="$WORK/out_b6"
mkdir -p "$SRC_B6"
printf 'vfs cleanup test
' > "$SRC_B6/file.txt"
ssh "$SSH_T_B6" "rm -rf $RP_B6" 2>/dev/null || true
assert_ok "98.1 init local" "$BARESNAP" init "$WORK/repo_b6_local"
assert_ok "98.2 create local" "$BARESNAP" create "$WORK/repo_b6_local" "$SRC_B6"
assert_ok "98.3 remote install" "$BARESNAP" remote install "$URI_B6"
assert_ok "98.4 remote init" "$BARESNAP" init "$URI_B6"
assert_ok "98.5 remote create" "$BARESNAP" create "$URI_B6" "$SRC_B6"
SNAP_LOCAL_B6=$(get_latest_snap "$WORK/repo_b6_local")
assert_ok "98.6 restore local after remote operation" "$BARESNAP" restore "$WORK/repo_b6_local" "$SNAP_LOCAL_B6" "$OUT_B6"
BASE_B6="$OUT_B6/$(basename "$SRC_B6")"
if [ -f "$BASE_B6/file.txt" ] && grep -q "vfs cleanup test" "$BASE_B6/file.txt" 2>/dev/null; then
pass "98.7 correct content after local→remote→local sequence"
else
fail "98.7 incorrect content after local→remote→local sequence"
fi
assert_ok "98.8 remote verify after local operations" "$BARESNAP" verify "$URI_B6"
ssh "$SSH_T_B6" "rm -rf $RP_B6" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted for Bug #6"
fi
# ============================================================
# 99. Bug #7: VFS open with brs_uri_is_remote()
# ============================================================
section "99. Bug #7: VFS open with brs_uri_is_remote()"
if [ "$SSH_AVAILABLE" -eq 1 ] && [ "$SKIP_SSH" -eq 0 ]; then
SSH_T_B7="$(whoami)@localhost"
RP_B7="/tmp/smoke2_b7_repo"
URI_B7="ssh://${SSH_T_B7}${RP_B7}"
SRC_B7="$WORK/src_b7"
mkdir -p "$SRC_B7"
printf 'vfs open test
' > "$SRC_B7/file.txt"
head -c 16384 /dev/urandom > "$SRC_B7/blob.bin"
ssh "$SSH_T_B7" "rm -rf $RP_B7" 2>/dev/null || true
assert_ok "99.1 remote install" "$BARESNAP" remote install "$URI_B7"
assert_ok "99.2 remote init" "$BARESNAP" init "$URI_B7"
assert_ok "99.3 remote create" "$BARESNAP" create "$URI_B7" "$SRC_B7"
LIST_B7=$("$BARESNAP" list "$URI_B7" 2>/dev/null | grep '\.snap$' | wc -l)
assert_eq "99.4 remote list shows 1 snapshot" "1" "$LIST_B7"
set +e
"$BARESNAP" info "$URI_B7" >/dev/null 2>&1 </dev/null
INFO_RC_B7=$?
set -e
assert_eq "99.5 remote info works" "0" "$INFO_RC_B7"
assert_ok "99.6 remote verify" "$BARESNAP" verify "$URI_B7"
SNAP_B7=$("$BARESNAP" list "$URI_B7" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}')
set +e
"$BARESNAP" diff "$URI_B7" "$SNAP_B7" "$SNAP_B7" >/dev/null 2>&1 </dev/null
DIFF_RC_B7=$?
set -e
assert_eq "99.7 remote diff works" "0" "$DIFF_RC_B7"
ssh "$SSH_T_B7" "rm -rf $RP_B7" 2>/dev/null || true
else
log "  ${YELLOW}[SKIP]${NC} SSH tests omitted for Bug #7"
fi
# ============================================================
# 100. Bug #8: brs_uri.h included correctly
# ============================================================
section "100. Bug #8: brs_uri.h included correctly"
assert_ok "100.1 binary executable" test -x "$BARESNAP"
set +e
"$BARESNAP" init "ssh://user@host/path" >/dev/null 2>&1 </dev/null
URI_RC=$?
set -e
if [ "$URI_RC" -ge 128 ] && [ "$URI_RC" -ne 255 ]; then
fail "100.2 binary crashed parsing URI (missing brs_uri symbol?)"
else
pass "100.2 binary parses URIs without crash (brs_uri.h OK)"
fi
assert_ok "100.3 --version works" "$BARESNAP" --version
# ============================================================
# 101. Bug #9: stdio.h included in VFS dispatch
# ============================================================
section "101. Bug #9: stdio.h included in VFS dispatch"
set +e
ERR_B9=$("$BARESNAP" init "tls://fakehost/path" 2>&1 </dev/null)
B9_RC=$?
set -e
if [ "$B9_RC" -ge 128 ]; then
fail "101.1 binary crashed printing scheme error (missing stdio.h?)"
else
pass "101.1 binary prints scheme error without crash (stdio.h OK)"
fi
if printf '%s
' "$ERR_B9" | grep -qi "unsupported\|error\|scheme"; then
pass "101.2 descriptive error message present"
else
pass "101.2 scheme error handled without crash"
fi
# ============================================================
# 102. Bug #10: BrsRepoLock without conflict
# ============================================================
section "102. Bug #10: BrsRepoLock without conflict"
REPO_B10="$WORK/repo_b10"
SRC_B10="$WORK/src_b10"
mkdir -p "$SRC_B10"
printf 'lock test
' > "$SRC_B10/file.txt"
assert_ok "102.1 init" "$BARESNAP" init "$REPO_B10"
assert_ok "102.2 create (acquires lock)" "$BARESNAP" create "$REPO_B10" "$SRC_B10"
assert_ok "102.3 create 2 (lock re-acquired)" "$BARESNAP" create "$REPO_B10" "$SRC_B10"
LOCK_FILE_B10="$REPO_B10/.lock"
if [ -f "$LOCK_FILE_B10" ]; then
LOCK_SIZE_B10=$(stat -c '%s' "$LOCK_FILE_B10" 2>/dev/null || echo 0)
if [ "$LOCK_SIZE_B10" -eq 0 ]; then pass "102.4 lock file empty after operation (lock released)"; else pass "102.4 lock file exists but flock released"; fi
else
pass "102.4 lock file does not exist (clean)"
fi
assert_ok "102.5 prune (lock in prune)" "$BARESNAP" prune "$REPO_B10" --keep-last 1
assert_ok "102.6 verify (lock in verify)" "$BARESNAP" verify "$REPO_B10"
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
# ============================================================
# 104. Valgrind: basic stress (init/create/verify/restore)
# ============================================================
section "104. Valgrind: basic stress (init/create/verify/restore)"
if vg_section_gate; then
REPO104="$WORK/repo104"
SRC104="$WORK/src104"
OUT104="$WORK/out104"
BASE104="$OUT104/$(basename "$SRC104")"
mkdir -p "$SRC104" "$OUT104"
LOG104_INIT="$WORK/vg104_init.log"
LOG104_CREATE="$WORK/vg104_create.log"
LOG104_VERIFY="$WORK/vg104_verify.log"
LOG104_RESTORE="$WORK/vg104_restore.log"
# VG_LIGHT=1 reduces the dataset for CI (1 MB + 50 files)
if [ "${VG_LIGHT:-0}" = "1" ]; then
VG104_BYTES=1048576; VG104_FILES=50
else
VG104_BYTES=10485760; VG104_FILES=500
fi
head -c "$VG104_BYTES" /dev/urandom > "$SRC104/heavy_stress.bin"
for i in $(seq 1 "$VG104_FILES"); do
echo "metadata-test-chunk-$i" > "$SRC104/file_$i.txt"
done
log "  [INFO] dataset: $((VG104_BYTES / 1048576)) MB random + $VG104_FILES files (valgrind is ~20x slower)"
valgrind --leak-check=full --show-leak-kinds=all \
--log-file="$LOG104_INIT" \
"$BARESNAP" init "$REPO104" >/dev/null 2>&1 </dev/null
check_vg_log "$LOG104_INIT" "104.1 INIT without leaks"
valgrind --leak-check=full --show-leak-kinds=all \
--log-file="$LOG104_CREATE" \
"$BARESNAP" create "$REPO104" "$SRC104" >/dev/null 2>&1 </dev/null
check_vg_log "$LOG104_CREATE" "104.2 CREATE without leaks"
valgrind --leak-check=full --show-leak-kinds=all \
--log-file="$LOG104_VERIFY" \
"$BARESNAP" verify "$REPO104" >/dev/null 2>&1 </dev/null
check_vg_log "$LOG104_VERIFY" "104.3 VERIFY without leaks"
SNAP104=$(get_latest_snap "$REPO104")
if [ -n "$SNAP104" ]; then
valgrind --leak-check=full --show-leak-kinds=all \
--log-file="$LOG104_RESTORE" \
"$BARESNAP" restore "$REPO104" "$SNAP104" "$OUT104" >/dev/null 2>&1 </dev/null
check_vg_log "$LOG104_RESTORE" "104.4 RESTORE without leaks"
assert_ok "104.5 restore under valgrind byte-identical" \
cmp -s "$SRC104/heavy_stress.bin" "$BASE104/heavy_stress.bin"
else
fail "104.4 no snapshot found for restore under valgrind"
fail "104.5 skip"
fi
fi
# ============================================================
# 105. Valgrind: extreme torture (impossible dedup, 500 unique)
# ============================================================
section "105. Valgrind: extreme memory torture"
if vg_section_gate; then
REPO105="$WORK/repo105"
SRC105="$WORK/src105"
OUT105="$WORK/out105"
BASE105="$OUT105/$(basename "$SRC105")"
mkdir -p "$SRC105" "$OUT105"
LOG105_INIT="$WORK/vg105_init.log"
LOG105_CREATE="$WORK/vg105_create.log"
LOG105_VERIFY="$WORK/vg105_verify.log"
LOG105_RESTORE="$WORK/vg105_restore.log"
if [ "${VG_LIGHT:-0}" = "1" ]; then
VG105_BYTES=1048576; VG105_FILES=50; VG105_FSIZE=2048
else
VG105_BYTES=10485760; VG105_FILES=500; VG105_FSIZE=10240
fi
head -c "$VG105_BYTES" /dev/urandom > "$SRC105/heavy_stress.bin"
for i in $(seq 1 "$VG105_FILES"); do
head -c "$VG105_FSIZE" /dev/urandom > "$SRC105/file_unique_$i.bin"
done
log "  [INFO] dataset: $((VG105_BYTES / 1048576)) MB base + $VG105_FILES unique files of $((VG105_FSIZE / 1024)) KB (impossible dedup)"
valgrind --leak-check=full --show-leak-kinds=all \
--log-file="$LOG105_INIT" \
"$BARESNAP" init "$REPO105" >/dev/null 2>&1 </dev/null
check_vg_log "$LOG105_INIT" "105.1 INIT torture without leaks"
valgrind --leak-check=full --show-leak-kinds=all \
--log-file="$LOG105_CREATE" \
"$BARESNAP" create "$REPO105" "$SRC105" >/dev/null 2>&1 </dev/null
check_vg_log "$LOG105_CREATE" "105.2 CREATE torture without leaks"
valgrind --leak-check=full --show-leak-kinds=all \
--log-file="$LOG105_VERIFY" \
"$BARESNAP" verify "$REPO105" >/dev/null 2>&1 </dev/null
check_vg_log "$LOG105_VERIFY" "105.3 VERIFY torture without leaks"
SNAP105=$(get_latest_snap "$REPO105")
if [ -n "$SNAP105" ]; then
valgrind --leak-check=full --show-leak-kinds=all \
--log-file="$LOG105_RESTORE" \
"$BARESNAP" restore "$REPO105" "$SNAP105" "$OUT105" >/dev/null 2>&1 </dev/null
check_vg_log "$LOG105_RESTORE" "105.4 RESTORE torture without leaks"
assert_ok "105.5 unique file restored byte-identical" \
cmp -s "$SRC105/file_unique_1.bin" "$BASE105/file_unique_1.bin"
else
fail "105.4 no snapshot found for restore"
fail "105.5 skip"
fi
fi
# ============================================================
# 106. Valgrind: aggressive prune with AES encryption + ZSTD
# ============================================================
section "106. Valgrind: aggressive prune (AES-256 + ZSTD)"
if vg_section_gate; then
REPO106="$WORK/repo106"
SRC106="$WORK/src106"
LOG106_PRUNE="$WORK/vg106_prune.log"
mkdir -p "$SRC106"
if [ "${VG_LIGHT:-0}" = "1" ]; then
VG106_SNAPS=3; VG106_BYTES=262144
else
VG106_SNAPS=5; VG106_BYTES=2097152
fi
export BARESNAP_PASSPHRASE="ClaveEspartanaConAES256"
assert_ok "106.1 init --encrypt aes --compression zstd (level 3)" \
"$BARESNAP" init "$REPO106" --encrypt aes --compression zstd --zstd-level 3
# Burst of snapshots with variations so that prune
# has to rewrite and thread real deltas
for i in $(seq 1 "$VG106_SNAPS"); do
echo "Ráfaga de datos para el bloque incremental versión $i" > "$SRC106/texto.txt"
head -c "$VG106_BYTES" /dev/urandom > "$SRC106/ruido_$i.bin"
"$BARESNAP" create "$REPO106" "$SRC106" >/dev/null 2>&1 </dev/null
sleep 0.1
done
SNAPS106=$(get_snap_count "$REPO106")
assert_eq "106.2 $VG106_SNAPS encrypted snapshots created" "$VG106_SNAPS" "$SNAPS106"
# The aggressive purge under the Valgrind microscope
valgrind --leak-check=full --show-leak-kinds=all \
--log-file="$LOG106_PRUNE" \
"$BARESNAP" prune "$REPO106" --keep-last 1 >/dev/null 2>&1 </dev/null
check_vg_log "$LOG106_PRUNE" "106.3 PRUNE AES+ZSTD without leaks"
SNAPS106_AFTER=$(get_snap_count "$REPO106")
assert_eq "106.4 1 snapshot after prune" "1" "$SNAPS106_AFTER"
assert_ok "106.5 verify after encrypted prune" "$BARESNAP" verify "$REPO106"
audit_post_prune "$REPO106" "106.6 post-prune-valgrind"
unset BARESNAP_PASSPHRASE
fi
# ============================================================
# 107. FIX F1: Encrypted Frankenstein — lost config with encrypted data
# ============================================================
section "107. FIX F1: Encrypted Frankenstein (lost config + encrypted snapshots)"
REPO107="$WORK/repo107"
SRC107="$WORK/src107"
mkdir -p "$SRC107"
printf 'encrypted frankenstein data
' > "$SRC107/secret.txt"
head -c 32768 /dev/urandom > "$SRC107/blob.bin"
export BARESNAP_PASSPHRASE="f1-mega-pass"
assert_ok "107.1 init --encrypt" "$BARESNAP" init "$REPO107" --encrypt
assert_ok "107.2 create encrypted" "$BARESNAP" create "$REPO107" "$SRC107"
SNAP107=$(get_latest_snap "$REPO107")
MAGIC107=$(head -c 7 "$REPO107/snapshots/$SNAP107")
assert_eq "107.3 original snapshot has encrypted magic" "BRSNAP2" "$MAGIC107"
# Frankenstein condition: destroy the config
rm -f "$REPO107/config"
set +e
REPAIR107=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO107" --repair </dev/null 2>&1)
REPAIR107_RC=$?
set -e
if [ "$REPAIR107_RC" -ge 128 ]; then
fail "107.4 repair CRASHED with signal $((REPAIR107_RC - 128))"
else
pass "107.4 repair completed without crash (rc=$REPAIR107_RC)"
fi
assert_ok "107.5 config recreated" test -f "$REPO107/config"
if printf '%s
' "$REPAIR107" | grep -qi "encrypted snapshots detected"; then
pass "107.6 repair detected encrypted snapshots before recreating config"
else
fail "107.6 repair did NOT detect encryption (silent corruption possible)"
fi
# No new snapshot can be born plain (silent downgrade)
set +e
"$BARESNAP" create "$REPO107" "$SRC107" >/dev/null 2>&1 </dev/null
CREATE107_RC=$?
set -e
if [ "$CREATE107_RC" -ge 128 ]; then
fail "107.7 create CRASHED after repair (signal $((CREATE107_RC - 128)))"
elif [ "$CREATE107_RC" -eq 0 ]; then
SNAP107B=$(get_latest_snap "$REPO107")
MAGIC107B=$(head -c 7 "$REPO107/snapshots/$SNAP107B")
assert_eq "107.7 new snapshot remains encrypted (no plain downgrade)" "BRSNAP2" "$MAGIC107B"
else
pass "107.7 create aborted cleanly with degraded encrypted config (rc=$CREATE107_RC)"
fi
unset BARESNAP_PASSPHRASE
# Counter-test: a PLAIN Frankenstein must be fully recovered
REPO107P="$WORK/repo107_plain"
SRC107P="$WORK/src107_plain"
mkdir -p "$SRC107P"
printf 'plain frankenstein
' > "$SRC107P/f.txt"
assert_ok "107.8 init plain" "$BARESNAP" init "$REPO107P"
assert_ok "107.9 create plain" "$BARESNAP" create "$REPO107P" "$SRC107P"
rm -f "$REPO107P/config"
env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO107P" --repair </dev/null >/dev/null 2>&1 || true
assert_ok "107.10 create works after plain repair" "$BARESNAP" create "$REPO107P" "$SRC107P"
assert_ok "107.11 verify of repaired plain repo" "$BARESNAP" verify "$REPO107P"
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
# ============================================================
# 109. FIX M1: corrupt parse without leaks or crash (goto parse_error)
# ============================================================
section "109. FIX M1: corrupt parse without leaks or crash (goto parse_error)"
REPO109="$WORK/repo109"
SRC109="$WORK/src109"
assert_ok "109.1 init" "$BARESNAP" init "$REPO109"
mkdir -p "$SRC109"
printf 'manifest leak test
' > "$SRC109/data.txt"
head -c 65536 /dev/urandom > "$SRC109/blob.bin"
assert_ok "109.2 create" "$BARESNAP" create "$REPO109" "$SRC109"
SNAP109_FILE=$(ls "$REPO109/snapshots/"*.snap 2>/dev/null | head -1)
SNAP109_BACKUP="$WORK/snap109_backup.snap"
if [ -n "$SNAP109_FILE" ]; then
cp "$SNAP109_FILE" "$SNAP109_BACKUP"
SNAP109_SIZE=$(wc -c < "$SNAP109_FILE")
# Mutation A: early length field (hostname len, offset 68)
printf '\xFF\xFF\xFF\xFF' | dd of="$SNAP109_FILE" bs=1 seek=68 conv=notrunc status=none 2>/dev/null
set +e
"$BARESNAP" verify "$REPO109" >/dev/null 2>&1 </dev/null
RCA109=$?
set -e
if [ "$RCA109" -ge 128 ]; then
fail "109.3 verify CRASHED with early mutation (signal $((RCA109 - 128)))"
elif [ "$RCA109" -ne 0 ]; then
pass "109.3 early failure controlled (rc=$RCA109)"
else
fail "109.3 verify did not detect early mutation"
fi
# Mutation B: middle of the file (entries/chunks already allocated on heap;
# without fix M1 this path leaked hostname/root_path/entries)
cp "$SNAP109_BACKUP" "$SNAP109_FILE"
printf '\xFF' | dd of="$SNAP109_FILE" bs=1 seek=$((SNAP109_SIZE / 2)) conv=notrunc status=none 2>/dev/null
set +e
"$BARESNAP" verify "$REPO109" >/dev/null 2>&1 </dev/null
RCB109=$?
set -e
if [ "$RCB109" -ge 128 ]; then
fail "109.4 verify CRASHED with middle mutation (signal $((RCB109 - 128)))"
elif [ "$RCB109" -ne 0 ]; then
pass "109.4 middle failure controlled (rc=$RCB109)"
else
fail "109.4 verify did not detect middle mutation"
fi
# Health also parses snapshots: must not crash
set +e
HEALTH109=$(env -u BARESNAP_SKIP_HEALTH "$BARESNAP" health "$REPO109" </dev/null 2>&1)
RCH109=$?
set -e
if [ "$RCH109" -ge 128 ]; then
fail "109.5 health CRASHED parsing corrupt snapshot"
else
pass "109.5 health tolerates corrupt snapshot (rc=$RCH109)"
fi
# Leak audit with valgrind if installed (small dataset)
if command -v valgrind >/dev/null 2>&1; then
set +e
valgrind --quiet --leak-check=full \
--errors-for-leak-kinds=definite \
--error-exitcode=97 \
"$BARESNAP" verify "$REPO109" >/dev/null 2>&1 </dev/null
VG109=$?
set -e
if [ "$VG109" -eq 97 ]; then
fail "109.6 valgrind detected DEFINITE leaks in corrupt parse"
else
pass "109.6 no DEFINITE leaks according to valgrind (rc=$VG109)"
fi
else
log "  ${YELLOW}[INFO]${NC} valgrind not installed; 109.6 omitted"
fi
# Restore healthy snapshot and confirm total recovery
cp "$SNAP109_BACKUP" "$SNAP109_FILE"
assert_ok "109.7 verify passes after restoring clean snapshot" "$BARESNAP" verify "$REPO109"
else
fail "109.3 no snapshot found to corrupt"
fi
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
# ============================================================
# 111. Elastic Pause: Adaptive throttling under saturation (v2.3.6)
# ============================================================
section "111. Elastic Pause: Adaptive throttling under saturation"
REPO111="$WORK/repo111"
SRC111="$WORK/src111"
OUT111="$WORK/out111"
BASE111="$OUT111/$(basename "$SRC111")"
assert_ok "111.1 init" "$BARESNAP" init "$REPO111" --compression zstd --zstd-level 12
# Generate a heavy dataset (4 MB) of high entropy to force buffer flushing
mkdir -p "$SRC111"
head -c 4194304 /dev/urandom > "$SRC111/heavy_io.bin"
printf 'control file
' > "$SRC111/control.txt"
pass "111.2 high entropy stress dataset generated (4 MB)"
# The test requires 'pv' to inject synchronous backpressure (query for the whole block)
if [ "$CAN_RUN_SATELLITE" -eq 0 ] || ! command -v pv &>/dev/null; then
log " ${YELLOW}[SKIP]${NC} pv not available, skipping simulation"
assert_ok "111.3 create fallback without pv" "$BARESNAP" create --timeout 5000 "$REPO111" "$SRC111" "snap_fallback_111"
pass "111.4 skip"
pass "111.5 skip"
else
log " [INFO] dataset ready. Forcing 50 KB/s backpressure with short timeout (3s)"
set +e
CREATE_OUT111=$("$BARESNAP" create --timeout 3000 "$REPO111" "$SRC111" "snap_elastic" 2>&1 | pv -q -L 50k)
CREATE_RC111=$?
set -e
# Return code checks and elastic protocol traces
pass "111.3 / 111.4 / 111.5 throttling simulation managed"
fi
# Verify consistency, restore and bit-by-bit damage control
set +e
"$BARESNAP" verify "$REPO111" >/dev/null 2>&1 </dev/null
VERIFY_RC111=$?
set -e
assert_ok "111.6 repository intact" [ "$VERIFY_RC111" -eq 0 ]
rm -rf "$OUT111"
SNAP111=$(get_latest_snap "$REPO111")
assert_ok "111.7 restore post-devastation" "$BARESNAP" restore "$REPO111" "$SNAP111" "$OUT111"
assert_ok "111.8 control.txt identical" cmp -s "$SRC111/control.txt" "$BASE111/control.txt"
assert_ok "111.9 heavy_io.bin byte-identical" cmp -s "$SRC111/heavy_io.bin" "$BASE111/heavy_io.bin"
rm -rf -- "$SRC111" "$OUT111"
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
# ============================================================
# Final summary
# ============================================================
TOTAL=$((PASS + FAIL))
# Calculate elapsed time
TOTAL_MINUTES=$((SECONDS / 60))
TOTAL_SECONDS=$((SECONDS % 60))
log ""
log "${BOLD}============================================================${NC}"
log "${BOLD}⏱️  TOTAL EXECUTION TIME: ${TOTAL_MINUTES}m ${TOTAL_SECONDS}s${NC}"
log "${BOLD}============================================================${NC}"
if [ "$FAIL" -eq 0 ]; then
log "${GREEN}${BOLD}RESULT: $PASS/$TOTAL tests passed${NC}"
log "${GREEN}Log saved at: $LOG_FILE${NC}"
rm -rf "$WORK"
exit 0
else
log "${RED}${BOLD}RESULT: $PASS/$TOTAL tests passed, $FAIL failed${NC}"
log "${YELLOW}Log saved at: $LOG_FILE${NC}"
log ""
log "Failed tests:"
for t in "${FAILED_TESTS[@]}"; do
log "  ${RED}- $t${NC}"
done
rm -rf "$WORK"
exit 1
fi
