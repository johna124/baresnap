#!/usr/bin/env bash
# ============================================================
# BareSnap - Regression test suite bootstrap
# ============================================================

export BARESNAP_SKIP_HEALTH=1
set -u

# ============================================================
# COLOR DEFINITION
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
# ARGUMENTS
# ============================================================
SKIP_SSH=0
SKIP_VALGRIND=0
HASH_OPT=""
HASH_MODE="fnv1a"

for arg in "$@"; do
    if [ "$arg" = "--no-ssh" ]; then SKIP_SSH=1; fi
    if [ "$arg" = "--no-valgrind" ]; then SKIP_VALGRIND=1; fi
    if [ "$arg" = "--blake2b" ]; then
        HASH_MODE="blake2b"
        HASH_OPT="--hash blake2b"
    fi
done

# ============================================================
# BareSnap binary
# ============================================================
BARESNAP="${BARESNAP:-./baresnap}"

# ============================================================
# Architecture detection
# ============================================================
ARCH=$(file "$BARESNAP" 2>/dev/null | awk '{
    if (match($0, /RISC-V|x86-64|AMD64|ARM|aarch64|PowerPC|MIPS|SPARC|i686/))
        print substr($0, RSTART, RLENGTH);
    else
        print "Unknow"
}')
ARCH=${ARCH:-Unknow}

if [ "$ARCH" = "Unknow" ] && command -v objdump >/dev/null 2>&1; then
    if objdump -d "$BARESNAP" 2>/dev/null | grep -Eq 'cmov|cmpxchg8b'; then
        ARCH="i686"
    fi
fi

# ============================================================
# QEMU detection
# ============================================================
QEMU_DETECTED=0
QEMU_REASON=""

if command -v systemd-detect-virt >/dev/null 2>&1; then
    VIRT_ID=$(systemd-detect-virt 2>/dev/null || true)
    case "$VIRT_ID" in
        qemu|qemu-user|kvm)
            QEMU_DETECTED=1
            QEMU_REASON="systemd-detect-virt=${VIRT_ID}"
            ;;
    esac
fi

if [ "$QEMU_DETECTED" -eq 0 ] && [ "$ARCH" != "Unknow" ]; then
    HOST_ARCH=$(uname -m 2>/dev/null || echo unknown)
    NATIVE=1
    case "$ARCH/$HOST_ARCH" in
        x86-64/x86_64|x86-64/amd64|AMD64/x86_64|AMD64/amd64) NATIVE=0 ;;
        i686/i686|i686/i[3-6]86|i686/x86_64|i686/amd64) NATIVE=0 ;;
        aarch64/aarch64|ARM/armv*|ARM/aarch64) NATIVE=0 ;;
        PowerPC/ppc*|PowerPC/powerpc*) NATIVE=0 ;;
        RISC-V/riscv*) NATIVE=0 ;;
        MIPS/mips*) NATIVE=0 ;;
        SPARC/sparc*) NATIVE=0 ;;
    esac

    if [ "$NATIVE" -eq 1 ]; then
        QEMU_DETECTED=1
        QEMU_REASON="binary arch ${ARCH} != host arch ${HOST_ARCH}, possible qemu-user"
    fi
fi

if [ "$QEMU_DETECTED" -eq 1 ]; then
    ARCH_LABEL="${ARCH} (QEMU)"
else
    ARCH_LABEL="${ARCH}"
fi

BOOTSTRAP_VERSION_TIMEOUT=2
if [ "$QEMU_DETECTED" -eq 1 ]; then
    BOOTSTRAP_VERSION_TIMEOUT=10
fi

# ============================================================
# HELPER FUNCTIONS (Defined BEFORE use)
# ============================================================

_bootstrap_cmd_out() {
    if command -v timeout >/dev/null 2>&1; then
        timeout "${BOOTSTRAP_VERSION_TIMEOUT:-2}" "$@" </dev/null 2>&1 | head -n 20 || true
    else
        "$@" </dev/null 2>&1 | head -n 20 || true
    fi
}

_bootstrap_slugify() {
    printf '%s' "$1" | awk '{
        gsub(/\+/, "-");
        gsub(/[^A-Za-z0-9._-]/, "_");
        printf "%s", $0;
    }'
}

_BOOTSTRAP_SAN_RESULT=""
_bootstrap_add_sanitizer() {
    local name="$1"
    case "$_BOOTSTRAP_SAN_RESULT" in
        *"$name"*) ;;
        *) _BOOTSTRAP_SAN_RESULT="${_BOOTSTRAP_SAN_RESULT:+$_BOOTSTRAP_SAN_RESULT+}$name" ;;
    esac
}

detect_sanitizers() {
    local bin="$1"
    _BOOTSTRAP_SAN_RESULT=""

    if command -v nm >/dev/null 2>&1; then
        local nm_out
        nm_out="$(nm -a "$bin" 2>/dev/null | grep -E '__asan_init|__lsan|__ubsan|__tsan_init|__msan_init' || true)"
        if printf '%s\n' "$nm_out" | grep -q '__asan_init'; then _bootstrap_add_sanitizer "ASan"; fi
        if printf '%s\n' "$nm_out" | grep -Eq '__lsan'; then _bootstrap_add_sanitizer "LSan"; fi
        if printf '%s\n' "$nm_out" | grep -Eq '__ubsan'; then _bootstrap_add_sanitizer "UBSan"; fi
        if printf '%s\n' "$nm_out" | grep -q '__tsan_init'; then _bootstrap_add_sanitizer "TSan"; fi
        if printf '%s\n' "$nm_out" | grep -q '__msan_init'; then _bootstrap_add_sanitizer "MSan"; fi
    fi

    if command -v strings >/dev/null 2>&1; then
        local str_out
        str_out="$(strings "$bin" 2>/dev/null | grep -Ei 'AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|MemorySanitizer' || true)"
        if printf '%s\n' "$str_out" | grep -qi 'AddressSanitizer'; then _bootstrap_add_sanitizer "ASan"; fi
        if printf '%s\n' "$str_out" | grep -qi 'LeakSanitizer'; then _bootstrap_add_sanitizer "LSan"; fi
        if printf '%s\n' "$str_out" | grep -qi 'UndefinedBehaviorSanitizer'; then _bootstrap_add_sanitizer "UBSan"; fi
        if printf '%s\n' "$str_out" | grep -qi 'ThreadSanitizer'; then _bootstrap_add_sanitizer "TSan"; fi
        if printf '%s\n' "$str_out" | grep -qi 'MemorySanitizer'; then _bootstrap_add_sanitizer "MSan"; fi
    fi

    if command -v ldd >/dev/null 2>&1; then
        local ldd_out
        ldd_out="$(ldd "$bin" 2>/dev/null || true)"
        if printf '%s\n' "$ldd_out" | grep -Eq 'libasan'; then _bootstrap_add_sanitizer "ASan"; fi
        if printf '%s\n' "$ldd_out" | grep -Eq 'liblsan'; then _bootstrap_add_sanitizer "LSan"; fi
        if printf '%s\n' "$ldd_out" | grep -Eq 'libubsan'; then _bootstrap_add_sanitizer "UBSan"; fi
        if printf '%s\n' "$ldd_out" | grep -Eq 'libtsan'; then _bootstrap_add_sanitizer "TSan"; fi
        if printf '%s\n' "$ldd_out" | grep -Eq 'libmsan'; then _bootstrap_add_sanitizer "MSan"; fi
    fi

    if [ -z "$_BOOTSTRAP_SAN_RESULT" ]; then
        echo "none"
    else
        echo "$_BOOTSTRAP_SAN_RESULT"
    fi
}

detect_baresnap_version() {
    local bin="$1"
    local out=""
    local arg
    local pattern='(BareSnap|baresnap|version).*(v?[0-9]+(\.[0-9]+)+)|^v?[0-9]+(\.[0-9]+)+'

    for arg in --version version -V -v; do
        out="$(_bootstrap_cmd_out "$bin" "$arg")"
        out="$(printf '%s\n' "$out" | grep -Ei -m1 "$pattern" || true)"
        if [ -n "$out" ]; then
            printf '%s' "$out"
            return 0
        fi
    done

    if command -v strings >/dev/null 2>&1; then
        out="$(strings "$bin" 2>/dev/null | grep -Ei -m1 "$pattern" || true)"
        if [ -n "$out" ]; then
            printf '%s' "$out"
            return 0
        fi
    fi

    echo "unknown"
}

# ============================================================
# Build LOG_FILE name
# ============================================================
BARESNAP_VERSION_RAW="unknown"
SANITIZER_BUILD_RAW="none"

if [ -x "$BARESNAP" ]; then
    BARESNAP_VERSION_RAW="$(detect_baresnap_version "$BARESNAP")"
    SANITIZER_BUILD_RAW="$(detect_sanitizers "$BARESNAP")"
fi

VERSION_TAG="$(_bootstrap_slugify "$BARESNAP_VERSION_RAW")"
[ -z "$VERSION_TAG" ] && VERSION_TAG="unknown"
VERSION_TAG="${VERSION_TAG:0:50}"

QEMU_TAG=""
[ "${QEMU_DETECTED:-0}" -eq 1 ] && QEMU_TAG="_qemu"

SAN_TAG=""
if [ "$SANITIZER_BUILD_RAW" != "none" ] && [ -n "$SANITIZER_BUILD_RAW" ]; then
    SAN_TAG="_$(_bootstrap_slugify "$SANITIZER_BUILD_RAW")"
fi

LOG_FILE="mega_test_${HASH_MODE}_${ARCH}${QEMU_TAG}_${VERSION_TAG}${SAN_TAG}.txt"
: > "$LOG_FILE"

# ============================================================
# Logging functions
# ============================================================
log_screen() { printf '%b\n' "$@"; }
log_file() { printf '%b\n' "$@" | sed 's/\x1b\[[0-9;]*m//g' >> "$LOG_FILE"; }
log() { log_screen "$@"; log_file "$@"; }


# ============================================================
# Initial boot log
# ============================================================
log "${YELLOW}✓ Architecture detected: ${ARCH_LABEL}${NC}"
if [ "$QEMU_DETECTED" -eq 1 ]; then
    log "${YELLOW}✓ QEMU detection: ${QEMU_REASON}${NC}"
fi

log "${YELLOW}✓ Binary: ${BARESNAP}${NC}"
log "${YELLOW}✓ BareSnap version: ${BARESNAP_VERSION_RAW}${NC}"

if [ "$SANITIZER_BUILD_RAW" = "none" ]; then
    log "${YELLOW}✓ Sanitizers: none (release build)${NC}"
elif [ "$SANITIZER_BUILD_RAW" = "unknown" ]; then
    log "${YELLOW}⚠ Sanitizers: unknown (binary missing or unreadable)${NC}"
else
    log "${GREEN}✓ Sanitizers: ${SANITIZER_BUILD_RAW}${NC}"
fi

[ -n "${ASAN_OPTIONS:-}" ] && log "${YELLOW}✓ ASAN_OPTIONS: ${ASAN_OPTIONS}${NC}"
[ -n "${UBSAN_OPTIONS:-}" ] && log "${YELLOW}✓ UBSAN_OPTIONS: ${UBSAN_OPTIONS}${NC}"
[ -n "${TSAN_OPTIONS:-}" ] && log "${YELLOW}✓ TSAN_OPTIONS: ${TSAN_OPTIONS}${NC}"
[ -n "${MSAN_OPTIONS:-}" ] && log "${YELLOW}✓ MSAN_OPTIONS: ${MSAN_OPTIONS}${NC}"
[ -n "${LSAN_OPTIONS:-}" ] && log "${YELLOW}✓ LSAN_OPTIONS: ${LSAN_OPTIONS}${NC}"

# ============================================================
# NUCLEAR CLEANUP IN /tmp
# ============================================================
rm -rf /tmp/.cache/baresnap 2>/dev/null || true
rm -rf /tmp/.config/baresnap 2>/dev/null || true
rm -rf /tmp/.local/share/baresnap 2>/dev/null || true

# ============================================================
# DEFINE WORK DIR
# ============================================================
WORK="$(mktemp -d /tmp/baresnap_tests.XXXXXX)"

export HOME="$WORK/fake_home"
export XDG_CACHE_HOME="$WORK/fake_home/.cache"
export XDG_CONFIG_HOME="$WORK/fake_home/.config"
export XDG_DATA_HOME="$WORK/fake_home/.local/share"

mkdir -p "$HOME" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"

trap 'rm -rf "$WORK"' EXIT

# ============================================================
# INITIALIZE GLOBAL TIME COUNTER & SESSION METADATA
# ============================================================
SECONDS=0
TEST_START_DATE=$(date "+%Y-%m-%d %H:%M:%S")
TEST_START_EPOCH=$(date "+%s")

log "${YELLOW}✓ Mega_Test session started on: ${TEST_START_DATE} (Epoch: ${TEST_START_EPOCH})${NC}"

if [ "$HASH_MODE" = "blake2b" ]; then
    log "${GREEN}${BOLD}✓ Hash Mode: BLAKE2B_128 Dynamic Cryptographic ACTIVE${NC}"
else
    log "${YELLOW}✓ Hash Mode: FNV1A Legacy Lightweight (default)${NC}"
fi

# ============================================================
# SSH PROBING
# ============================================================
SSH_AVAILABLE=0
if [ "$SKIP_SSH" -eq 0 ]; then
    if command -v ssh >/dev/null 2>&1; then
        if ssh -o BatchMode=yes -o ConnectTimeout=5 "$(whoami)@localhost" "echo ok" >/dev/null 2>&1; then
            SSH_AVAILABLE=1
        fi
    fi
fi

# ============================================================
# Valgrind detection
# ============================================================
VALGRIND_AVAILABLE=0
command -v valgrind >/dev/null 2>&1 && VALGRIND_AVAILABLE=1

if [ "$VALGRIND_AVAILABLE" -eq 1 ]; then
    if [ "$SKIP_VALGRIND" -eq 1 ]; then
        log "${YELLOW}⚠ Valgrind modules disabled (--no-valgrind)${NC}"
    else
        log "${YELLOW}✓ Valgrind available: memory modules ACTIVE by default${NC}"
    fi
else
    log "${YELLOW}⚠ Valgrind not installed: memory modules will be skipped${NC}"
    log "  (Install: apt install valgrind / dnf install valgrind)"
fi

# ============================================================
# Detection of optional dependencies
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
    log "${YELLOW}✓ Optional dependencies (pv, nc, strace, faketime, valgrind) available${NC}"
    CAN_RUN_SATELLITE=1
fi

# ============================================================
# TEST FRAMEWORK BEGINS
# ============================================================
PASS=0
FAIL=0
FAILED_TESTS=()
CURRENT_TEST=0

TOTAL_TESTS=$(cat "${MEGA_REFTEST_HOME:-.}/lib/00_bootstrap.sh" "${MEGA_SUITE_DIR:-.}"/*.sh 2>/dev/null \
    | grep -E '^[[:space:]]*section[[:space:]]+"' \
    | grep -v '^[[:space:]]*#' \
    | awk '{print $2}' \
    | sort -u \
    | wc -l)

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

    if [[ " $* " == *" init "* ]]; then
        local repo_dir=""
        for arg in "$@"; do
            if [[ "$arg" == *"/tmp/"* || "$arg" == *"/repo"* ]]; then repo_dir="$arg"; fi
        done
        [ -z "$repo_dir" ] && repo_dir="${@: -1}"

        if [ -n "$HASH_OPT" ]; then
            out="$("$@" --hash "$HASH_MODE" 2>&1)"
            rc=$?
        else
            out="$("$@" 2>&1)"
            rc=$?
        fi

        if [ $rc -eq 0 ] && [ -n "$HASH_OPT" ]; then
            if "$1" info "$repo_dir" 2>&1 | grep -Eiq 'BLAKE2B_128|hash:.*blake2'; then
                desc="$desc [VERIFIED: BLAKE2B REAL ON THE REPO]"
            else
                desc="$desc [ALERT: MUTATION FAILED – RESULTED IN FNV1A]"
                rc=1
            fi
        fi
    else
        out="$("$@" 2>&1)"
        rc=$?
    fi

    if [ "$rc" -ne 0 ]; then
        fail "$desc (init rc=$rc)"
        printf '%s\n' "$out" | head -n 5 | sed 's/^/[INIT] /' >>"$LOG_FILE"
        return 1
    fi

    if printf '%s\n' "$out" | grep -Eiq 'AES[- _]?256[- _]?GCM|Encryption:.*AES|cipher:.*aes'; then
        pass "$desc"
        return 0
    fi

    fail "$desc (init OK but DID NOT select AES)"
    printf '%s\n' "$out" | head -n 5 | sed 's/^/[INIT] /' >>"$LOG_FILE"
    return 1
}

assert_ok() {
    local desc="$1"
    shift
    local rc

    if [[ " $* " == *" init "* ]]; then
        local repo_dir=""
        for arg in "$@"; do
            if [[ "$arg" == *"/tmp/"* || "$arg" == *"/repo"* ]]; then repo_dir="$arg"; fi
        done
        [ -z "$repo_dir" ] && repo_dir="${@: -1}"

        if [ -n "$HASH_OPT" ] && [[ " $* " != *" health "* ]] && [[ " $* " != *" --pass-fd "* ]]; then
            "$@" --hash "$HASH_MODE" >/dev/null 2>&1 </dev/null
            rc=$?
        else
            "$@" >/dev/null 2>&1 </dev/null
            rc=$?
        fi

        if [ $rc -eq 0 ] && [ -n "$HASH_OPT" ] && [[ " $* " != *" health "* ]] && [[ " $* " != *" faketime "* ]] && [[ " $* " != *" --pass-fd "* ]]; then
            if "$1" info "$repo_dir" 2>&1 | grep -Eiq 'BLAKE2B_128|hash:.*blake2'; then
                desc="$desc [VERIFIED: BLAKE2B REAL ON THE REPO]"
            else
                desc="$desc [ALERT: MUTATION FAILED – RESULTED IN FNV1A]"
                rc=1
            fi
        fi
    else
        "$@" >/dev/null 2>&1 </dev/null
        rc=$?
    fi

    if [ $rc -eq 0 ]; then pass "$desc"; else fail "$desc"; fi
}

assert_eq() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if [ "$expected" = "$actual" ]; then pass "$desc"; else fail "$desc (expected='$expected' actual='$actual')"; fi
}

assert_info_aes() {
    local desc="$1"
    shift
    local out rc
    out="$("$@" 2>&1)"
    rc=$?

    if [ "$rc" -ne 0 ]; then
        fail "$desc (info rc=$rc)"
        printf '%s\n' "$out" | head -n 8 | sed 's/^/    [INFO] /' >>"$LOG_FILE"
        return 1
    fi

    if printf '%s\n' "$out" | grep -Eiq 'AES[- _]?256[- _]?GCM|Encryption:.*AES|encryption:.*AES'; then
        pass "$desc"
        return 0
    fi

    fail "$desc (info DOES NOT show AES)"
    printf '%s\n' "$out" | head -n 8 | sed 's/^/    [INFO] /' >>"$LOG_FILE"
    return 1
}

section() {
    CURRENT_TEST=$((CURRENT_TEST + 1))

    set +e
    set +o pipefail 2>/dev/null || true

    log ""
    log "${BOLD}== [$CURRENT_TEST/$TOTAL_TESTS] $1 ==${NC}"
}

get_snap_count() { "$BARESNAP" list "$1" 2>/dev/null | grep '\.snap$' | wc -l; }
get_latest_snap() { "$BARESNAP" list "$1" 2>/dev/null | grep '\.snap$' | tail -n1 | awk '{print $NF}'; }

if [ ! -x "$BARESNAP" ]; then
    log "${RED}ERROR: binary not found or not executable: $BARESNAP${NC}"
    log "Compile first: cmake -B build && cmake --build build"
    log "Or point to your binary: BARESNAP=/path/to/baresnap $0"
    exit 1
fi


section "Setup"
log ""
log "${BOLD}== Setup ==${NC}"
log "binary: $BARESNAP"
log "version: $BARESNAP_VERSION_RAW"
log "sanitizers: $SANITIZER_BUILD_RAW"
log "workdir: $WORK"
log "log file: $LOG_FILE"

if [ "$SSH_AVAILABLE" -eq 1 ]; then
    log "${GREEN}✓ SSH available${NC}"
elif [ "$SKIP_SSH" -eq 1 ]; then
    log "${YELLOW}⚠ SSH tests disabled (--no-ssh)${NC}"
else
    log "${YELLOW}⚠ SSH not available or no connectivity${NC}"
    log "   Configure: ssh-keygen && ssh-copy-id $(whoami)@localhost"
    log "   Or run: $0 --no-ssh"
fi
