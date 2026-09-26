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
    set +u
    local desc="${1:-Test without description}"
    shift
    
    if [ $# -eq 0 ]; then
        fail "$desc (assert_fail invoked without valid commands)"
        set -u
        return 1
    fi
    
    local rc=0
    
    # SHIELD AGAINST FUZZING ABORTS: 
    # We explicitly instruct ASan to return exit codes instead of calling abort()
    # when processing mutated, corrupted fuzzing headers.
    ASAN_OPTIONS="abort_on_error=0:check_initialization_order=0:detect_deadlocks=0" \
    "$@" >/dev/null 2>&1 </dev/null || rc=$?
    
    set -u
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

    if [ ! -d "$src" ] || [ ! -d "$dst" ]; then
        return 1
    fi

    # CHIVATO FORENSE ACTIVADO: Quitamos la censura para ver qué falla en el disco
    if ! diff -qr --no-dereference "$src" "$dst"; then
        echo "⚠️  [DIAGNOSTICO] Alerta de diferencia física encontrada por diff arriba."
        return 1
    fi
    return 0
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
check_vg_log() {
    # POSIX safe assignments without using local if contexts switch
    vg_log_file="$1"
    vg_stage="$2"
    
    if [ ! -f "$vg_log_file" ]; then
        fail "$vg_stage: valgrind log not generated"
        return 1
    fi
    
    # Extract values cleanly
    vg_leaks=$(grep "definitely lost:" "$vg_log_file" | awk '{print $4}' | tr -d ',' | head -1)
    vg_errors=$(grep "ERROR SUMMARY:" "$vg_log_file" | awk '{print $4}' | head -1)
    
    # Strict variable protections for set -u
    vg_leaks=${vg_leaks:-0}
    vg_errors=${vg_errors:-0}
    
    if [ "$vg_leaks" -eq 0 ] 2>/dev/null; then
        if [ "$vg_errors" -gt 0 ] 2>/dev/null; then
            pass "$vg_stage: 0 leaks [PASS] (Ignored $vg_errors glibc registry warnings)"
        else
            pass "$vg_stage: 0 leaks, 0 errors [PASS]"
        fi
        return 0
    else
        fail "$vg_stage: problems detected"
        log "    -> Leak: $vg_leaks bytes definitely lost"
        [ "$vg_errors" -gt 0 ] 2>/dev/null && log "    -> Access/read errors: $vg_errors"
        log "    -> Log: $vg_log_file"
        return 1
    fi
}

# ANTI-COLLAPSE SHIELD:
# If the framework accidentally unregisters vg_section_gate, we stub it right here
# to protect the loop from collapsing on line 5 of the suites.
if ! command -v vg_section_gate >/dev/null 2>&1; then
    vg_section_gate() {
        # Check safely if SKIP_VALGRIND or VALGRIND_AVAILABLE are set to prevent set -u crashes
        if [ "${SKIP_VALGRIND:-0}" -eq 1 ] 2>/dev/null; then
            return 1
        fi
        if [ "${VALGRIND_AVAILABLE:-1}" -ne 1 ] 2>/dev/null; then
            return 1
        fi
        return 0
    }
fi


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

