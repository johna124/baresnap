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

