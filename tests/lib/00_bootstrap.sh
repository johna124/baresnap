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
#rm -rf /tmp/baresnap* 2>/dev/null || true
#rm -rf /tmp/brs_* 2>/dev/null || true
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
# CONTROL DE ARGUMENTOS DEL MEGATEST --no-ssh / --no-valgrind  
#  --blake2b
# ============================================================

SKIP_SSH=0
SKIP_VALGRIND=0
HASH_OPT=""
HASH_MODE="fnv1a"

# Capturamos los flags directamente antes de que el script los limpie
for arg in "$@"; do
    if [ "$arg" = "--no-ssh" ]; then SKIP_SSH=1; fi
    if [ "$arg" = "--no-valgrind" ]; then SKIP_VALGRIND=1; fi
    if [ "$arg" = "--blake2b" ]; then
        HASH_MODE="blake2b"
        HASH_OPT="--hash blake2b"
    fi
done

# Colores nativos para el letrero visual
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
BOLD='\033[1m'

if [ "$HASH_MODE" = "blake2b" ]; then
    log "${GREEN}${BOLD}✓ Modo de Hash: BLAKE2B_128 Criptográfico Dinámico ACTIVE${NC}\n"
else
    log "${YELLOW}✓ Modo de Hash: FNV1A Legado Ligero por defecto${NC}\n"
fi
# ============================================================
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
TOTAL_TESTS=$(cat "$MEGA_REFTEST_HOME"/lib/00_bootstrap.sh "$MEGA_SUITE_DIR"/*.sh 2>/dev/null | grep -E '^[[:space:]]*section[[:space:]]+"' | grep -v '^[[:space:]]*#' | awk '{print $2}' | sort -u | wc -l)
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
            if [[ "$arg" == *"/tmp/"* || "$arg" == *"/repo"* ]]; then
                repo_dir="$arg"
            fi
        done
        if [ -z "$repo_dir" ]; then repo_dir="${@: -1}"; fi

        if [ -n "$HASH_OPT" ]; then
            out="$("$@" --hash "$HASH_MODE" 2>&1)"
            rc=$?
        else
            out="$("$@" 2>&1)"
            rc=$?
        fi
        
        # AUDITORÍA EN ENTORNO CIFRADO
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
    
    # 1. Si el comando contiene "init", inyectamos el flag oficial ordenado
    if [[ " $* " == *" init "* ]]; then
        local repo_dir=""
        for arg in "$@"; do
            if [[ "$arg" == *"/tmp/"* || "$arg" == *"/repo"* ]]; then
                repo_dir="$arg"
            fi
        done
        if [ -z "$repo_dir" ]; then repo_dir="${@: -1}"; fi

        # Bloqueamos la inyección si viene de un comando de salud 'health'
        if [ -n "$HASH_OPT" ] && [[ " $* " != *" health "* ]]; then
            "$@" --hash "$HASH_MODE" >/dev/null 2>&1 </dev/null
            rc=$?
        else
            "$@" >/dev/null 2>&1 </dev/null
            rc=$?
        fi

        # AUDITORÍA FORENSE: Validamos únicamente si no es un comando de caos/health
        if [ $rc -eq 0 ] && [ -n "$HASH_OPT" ] && [[ " $* " != *" health "* ]] && [[ " $* " != *" faketime "* ]]; then
            if "$1" info "$repo_dir" 2>&1 | grep -Eiq 'BLAKE2B_128|hash:.*blake2'; then
                desc="$desc [VERIFIED: BLAKE2B REAL ON THE REPO]"
            else
                desc="$desc [ALERT: MUTATION FAILED – RESULTED IN FNV1A]"
                rc=1
            fi
        fi
    else
        # Para create, restore, verify, health y evitar el deadlock de la 65, corre 100% de fábrica
        "$@" >/dev/null 2>&1 </dev/null
        rc=$?
    fi

    if [ $rc -eq 0 ]; then pass "$desc"; else fail "$desc"; fi
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
log "binary: $BARESNAP"
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

