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
    printf 'handle churn test\n' > "$SSH_SRC110/a.txt"
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
    printf 'churn v2\n' >> "$SSH_SRC110/a.txt"
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

    # ============================================================
    # STRESS TESTS: Handle churn torture (110.13 - 110.24)
    # Ejercita allocate/get/release_handle hasta el límite
    # ============================================================

    # Preparar fuentes de stress: 2000 ficheros pequeños
    STRESS_SRC110="$WORK/stress_src_r1"
    STRESS_OUT110="$WORK/stress_out_r1"
    rm -rf "$STRESS_SRC110" "$STRESS_OUT110"
    mkdir -p "$STRESS_SRC110"
    for i in $(seq 1 2000); do
        echo "churn_stress_data_$i" > "$STRESS_SRC110/file_$i.txt"
    done

    # 110.13: Backup de 2000 ficheros (churn OPEN/READ/CLOSE masivo)
    assert_ok "110.13 stress create 2000 files" "$BARESNAP" create "$SSH_URI110" "$STRESS_SRC110"
    SNAP110_STRESS=$(get_latest_snap "$SSH_URI110")

    # 110.14: Ráfaga de list (50x) — churn de opendir/closedir
    LIST_BURST_OK=1
    for i in $(seq 1 50); do
        "$BARESNAP" list "$SSH_URI110" >/dev/null 2>&1 || LIST_BURST_OK=0
    done
    assert_eq "110.14 burst 50x list" "1" "$LIST_BURST_OK"

    # 110.15: Ráfaga de info (50x) — churn de STAT handles
    INFO_BURST_OK=1
    for i in $(seq 1 50); do
        "$BARESNAP" info "$SSH_URI110" >/dev/null 2>&1 || INFO_BURST_OK=0
    done
    assert_eq "110.15 burst 50x info" "1" "$INFO_BURST_OK"

    # 110.16: Restore de 2000 ficheros (churn WRITE/CLOSE masivo)
    assert_ok "110.16 stress restore 2000 files" "$BARESNAP" restore "$SSH_URI110" "$SNAP110_STRESS" "$STRESS_OUT110"
    STRESS_BASE110="$STRESS_OUT110/$(basename "$STRESS_SRC110")"
    RESTORED_COUNT=$(find "$STRESS_BASE110" -name "file_*.txt" 2>/dev/null | wc -l)
    assert_eq "110.17 restored count = 2000" "2000" "$RESTORED_COUNT"

    # 110.18: Verificación de contenido (sample de 5 ficheros)
    CONTENT_OK=1
    for i in 1 500 1000 1500 2000; do
        EXPECTED="churn_stress_data_$i"
        ACTUAL=$(cat "$STRESS_BASE110/file_$i.txt" 2>/dev/null)
        if [ "$ACTUAL" != "$EXPECTED" ]; then
            CONTENT_OK=0
        fi
    done
    assert_eq "110.18 content integrity (sample)" "1" "$CONTENT_OK"

    # 110.19: Segundo ciclo de backup (re-churn tras 100+ operaciones)
    echo "new_data_cycle2" > "$STRESS_SRC110/file_2001.txt"
    assert_ok "110.19 stress create cycle 2" "$BARESNAP" create "$SSH_URI110" "$STRESS_SRC110"

    # 110.20: Verify tras churn intenso
    assert_ok "110.20 verify post-churn" "$BARESNAP" verify "$SSH_URI110"

    # 110.21: Interleave de operaciones (list/info/verify x10)
    INTERLEAVE_OK=1
    for i in $(seq 1 10); do
        "$BARESNAP" list "$SSH_URI110" >/dev/null 2>&1 || INTERLEAVE_OK=0
        "$BARESNAP" info "$SSH_URI110" >/dev/null 2>&1 || INTERLEAVE_OK=0
        "$BARESNAP" verify "$SSH_URI110" --fast >/dev/null 2>&1 || INTERLEAVE_OK=0
    done
    assert_eq "110.21 interleave list/info/verify x10" "1" "$INTERLEAVE_OK"

    # 110.22: Prune agresivo (re-churn de handles de packs)
    assert_ok "110.22 prune --keep-last 1" "$BARESNAP" prune "$SSH_URI110" --keep-last 1 --keep-daily 0 --keep-weekly 0 --keep-monthly 0 --keep-yearly 0

    # 110.23: Health check post-tortura
    assert_ok "110.23 health post-torture" "$BARESNAP" health "$SSH_URI110"

    # 110.24: Verify final (integridad total tras tortura)
    assert_ok "110.24 final verify post-torture" "$BARESNAP" verify "$SSH_URI110"

    # Limpieza final
    ssh "$SSH_TARGET110" "rm -rf $REMOTE_PATH110" 2>/dev/null || true
else
    section "110. FIX R1: remote agent — handle churn"
    log "  ${YELLOW}[SKIP]${NC} SSH tests omitted"
fi
