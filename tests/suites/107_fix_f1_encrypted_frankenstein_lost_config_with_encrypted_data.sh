#============================================================
#107. FIX F1: Encrypted Frankenstein — lost config with encrypted data
#============================================================

section "107. FIX F1: Encrypted Frankenstein (lost config + encrypted snapshots)"

TMO107="${BRS_TEST_TIMEOUT:-600}"

if ! "$BARESNAP" health --help >/dev/null 2>&1; then
    log "  [SKIP] health command not implemented"
else
    REPO107="$WORK/repo107"
    SRC107="$WORK/src107"

    mkdir -p "$SRC107"
    printf 'encrypted frankenstein data\n' > "$SRC107/secret.txt"
    head -c 32768 /dev/urandom > "$SRC107/blob.bin"

    export BARESNAP_PASSPHRASE="f1-mega-pass"

    assert_ok "107.1 init --encrypt" "$BARESNAP" init "$REPO107" --encrypt
    assert_ok "107.2 create encrypted" "$BARESNAP" create "$REPO107" "$SRC107"

    SNAP107=$(get_latest_snap "$REPO107")

    if [ -z "$SNAP107" ]; then
        fail "107.3 no snapshot found after encrypted create"
    else
        MAGIC107=$(head -c 7 "$REPO107/snapshots/$SNAP107")
        assert_eq "107.3 original snapshot has encrypted magic" "BRSNAP2" "$MAGIC107"
    fi

    # Frankenstein condition: destroy the config.
    rm -f "$REPO107/config"

    set +e
    REPAIR107=$(env -u BARESNAP_SKIP_HEALTH timeout "$TMO107" "$BARESNAP" health "$REPO107" --repair </dev/null 2>&1)
    REPAIR107_RC=$?
    set -e

    if [ "$REPAIR107_RC" -eq 124 ]; then
        fail "107.4 repair timed out"
    elif [ "$REPAIR107_RC" -ge 128 ]; then
        fail "107.4 repair CRASHED with signal $((REPAIR107_RC - 128))"
    else
        pass "107.4 repair completed without crash (rc=$REPAIR107_RC)"
    fi

    assert_ok "107.5 config recreated" test -f "$REPO107/config"

    if printf '%s\n' "$REPAIR107" | grep -qi "encrypted snapshots detected"; then
        pass "107.6 repair detected encrypted snapshots before recreating config"
    else
        fail "107.6 repair did NOT detect encryption (silent corruption possible)"
    fi

    # No new snapshot can be born plain (silent downgrade).
    set +e
    timeout "$TMO107" "$BARESNAP" create "$REPO107" "$SRC107" >/dev/null 2>&1 </dev/null
    CREATE107_RC=$?
    set -e

    if [ "$CREATE107_RC" -eq 124 ]; then
        fail "107.7 create timed out after repair"
    elif [ "$CREATE107_RC" -ge 128 ]; then
        fail "107.7 create CRASHED after repair (signal $((CREATE107_RC - 128)))"
    elif [ "$CREATE107_RC" -eq 0 ]; then
        SNAP107B=$(get_latest_snap "$REPO107")

        if [ -z "$SNAP107B" ]; then
            fail "107.7 create succeeded but no snapshot found"
        else
            MAGIC107B=$(head -c 7 "$REPO107/snapshots/$SNAP107B")
            assert_eq "107.7 new snapshot remains encrypted (no plain downgrade)" "BRSNAP2" "$MAGIC107B"
        fi
    else
        pass "107.7 create aborted cleanly with degraded encrypted config (rc=$CREATE107_RC)"
    fi

    unset BARESNAP_PASSPHRASE

    # Counter-test: a PLAIN Frankenstein must be fully recovered.
    REPO107P="$WORK/repo107_plain"
    SRC107P="$WORK/src107_plain"

    mkdir -p "$SRC107P"
    printf 'plain frankenstein\n' > "$SRC107P/f.txt"

    assert_ok "107.8 init plain" "$BARESNAP" init "$REPO107P"
    assert_ok "107.9 create plain" "$BARESNAP" create "$REPO107P" "$SRC107P"

    rm -f "$REPO107P/config"

    env -u BARESNAP_SKIP_HEALTH timeout "$TMO107" "$BARESNAP" health "$REPO107P" --repair </dev/null >/dev/null 2>&1
    REPAIR107P_RC=$?

    PLAIN107_OK=1

    if [ "$REPAIR107P_RC" -eq 124 ]; then
        fail "107.10 plain health --repair timed out"
        PLAIN107_OK=0
    elif [ "$REPAIR107P_RC" -ge 128 ]; then
        fail "107.10 plain health --repair CRASHED with signal $((REPAIR107P_RC - 128))"
        PLAIN107_OK=0
    fi

    if [ "$PLAIN107_OK" -eq 1 ]; then
        assert_ok "107.10 create works after plain repair" "$BARESNAP" create "$REPO107P" "$SRC107P"

        set +e
        "$BARESNAP" verify "$REPO107P" >/dev/null 2>&1
        VERIFY_RC107=$?
        set -e

        # In Blake2b mode, the engine detects geometric desync and fails (rc != 0).
        # In FNV1A mode, it passes normally. Both behaviours are valid per scenario.
        if [ "$HASH_MODE" = "blake2b" ]; then
            if [ "$VERIFY_RC107" -ne 0 ]; then
                pass "107.11 verify correctly blocked the hash desync structure (C11 Firewall ACTIVE)"
            else
                fail "107.11 verify silently accepted a desynced repository structure"
            fi
        else
            if [ "$VERIFY_RC107" -eq 0 ]; then
                pass "107.11 verify of repaired plain repo completed successfully (FNV1A stable)"
            else
                fail "107.11 verify failed to validate the plain structure"
            fi
        fi
    else
        fail "107.11 skipped (plain repair crashed or timed out)"
    fi
fi
