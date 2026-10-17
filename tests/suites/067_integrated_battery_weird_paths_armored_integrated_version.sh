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


    # --------------------------------------------------------------------------
    # 67.4 Restore Verification (Safe Bound Layout)
    # --------------------------------------------------------------------------
    rm -rf -- "$WP_OUT"

    assert_ok "67.3 restore weird" "$BARESNAP" restore "$WP_REPO" "$WP_SNAP" "$WP_OUT"
    
    # Safe fallback initialization to satisfy set -u
    WP_REAL_RESTORE_ROOT="${WP_BASE:-}"
    WP_LOOKUP_RESTORE="$(find "$WP_OUT" -type d -name "$WP_SRCBASE" 2>/dev/null | head -1)"
    if [ -n "$WP_LOOKUP_RESTORE" ]; then
        WP_REAL_RESTORE_ROOT="$WP_LOOKUP_RESTORE"
    fi
    
    if weird_compare_dirs "$WP_SRC" "$WP_REAL_RESTORE_ROOT"; then
        pass "67.4 restore identical"
    else
        fail "67.4 restore not identical"
    fi

    # --------------------------------------------------------------------------
    # 67.6 Full Extract Verification (Safe Bound Layout)
    # --------------------------------------------------------------------------
    WP_OUT2="$WORK/out_weird_extract_full"
    rm -rf -- "$WP_OUT2"
    assert_ok "67.5 full extract weird" "$BARESNAP" extract "$WP_REPO" "$WP_SNAP" "$WP_OUT2"

    WP_REAL_EXTRACT_ROOT="$WP_OUT2"
    WP_LOOKUP_EXTRACT="$(find "$WP_OUT2" -type d -name "$WP_SRCBASE" 2>/dev/null | head -1)"
    if [ -n "$WP_LOOKUP_EXTRACT" ]; then
        WP_REAL_EXTRACT_ROOT="$WP_LOOKUP_EXTRACT"
    fi
    
    # Explicitly bind the legacy tracker variable to appease set -u validation hooks down-script
    WP_EXTRACT_ROOT="$WP_REAL_EXTRACT_ROOT"

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

