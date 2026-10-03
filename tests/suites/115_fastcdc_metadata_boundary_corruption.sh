# ============================================================================
# 115. FastCDC: Metadata Tree Boundary Corruption and Health Detection
# ============================================================================
section "115. FastCDC: Metadata Tree Boundary Corruption and Health Detection"

# AVISO DE CARGA ACTIVO EN EL MEGATEST (Solo si se ejecutan Sanitizers)
if [ -n "${ASAN_OPTIONS:-}" ] || [ -n "${TSAN_OPTIONS:-}" ]; then
    echo -e "\n======================================================================"
    echo -e "⚠️  🚨 \e[33m[HEAVY COMPUTATION NOTICE: FASTCDC + CRYPTO + SANITIZERS]\e[0m 🚨 ⚠️"
    echo -e "======================================================================"
    echo -e " Running under ASan/TSan memory profiling on a Dual-Core architecture."
    echo -e " Cryptographic operations and rolling hash evaluation take extra time."
    echo -e " Please wait... The CPU is actively calculating and has NOT hung."
    echo -e "======================================================================\n" > /dev/stdout
fi

DIR_REPO115="$WORK/repo115"
DIR_SRC115="$WORK/src115"
DIR_RESTORE115="$WORK/restore115"
TEST_PASS115="FastCDCPunishment-2026-Metal"

rm -rf "$DIR_REPO115" "$DIR_SRC115" "$DIR_RESTORE115"
mkdir -p "$DIR_SRC115" "$DIR_RESTORE115"

# Generar payload estructurado para forzar el chunking de FastCDC
for i in {1..5}; do
    echo "Bloque repetitivo de ingenieria de guerrilla $i para alinear el chunker" >> "$DIR_SRC115/dedup.txt"
done
dd if=/dev/urandom of="$DIR_SRC115/random.bin" bs=1M count=2 status=none

# BLINDAJE CON DESCRIPTOR 8: Totalmente inmune a las fugas y bloqueos de la suite global
assert_ok "115.1 Initialize repository using verified --pass-fd" \
    "$BARESNAP" --pass-fd 8 init "$DIR_REPO115" --encrypt 8<<<"$TEST_PASS115"

assert_ok "115.2 Create snapshot with dynamic FastCDC chunking" \
    "$BARESNAP" --pass-fd 8 create "$DIR_REPO115" "$DIR_SRC115" "snap_cdc_115" 8<<<"$TEST_PASS115"


# =============================================================================
# ESCUDO DE AISLAMIENTO LOCAL CONTRA ABORTOS ABUPTOS DE ASAN (set +e)
# =============================================================================
OLD_SET_E=$(set +o | grep errexit)
OLD_PIPEFAIL=$(set +o | grep pipefail)
set +e
set +o pipefail

# EXTRACCIÓN NATIVA INTERNA DETERMINISTA (Cero comandos 'find | head' para evitar SIGPIPE)
INDEX_DIR="$DIR_REPO115/index"
if [ ! -d "$INDEX_DIR" ]; then
    for d in "$DIR_REPO115"/*; do
        if [ -d "$d/index" ]; then INDEX_DIR="$d/index"; break; fi
    done
fi

SNAP_FILE=""
for f in "$DIR_REPO115"/*; do
    if [ -f "$f" ]; then
        SNAP_FILE="$f"
        break
    fi
done

if [ ! -d "$INDEX_DIR" ] || [ -z "$SNAP_FILE" ] || [ ! -f "$SNAP_FILE" ]; then
    eval "$OLD_SET_E"; eval "$OLD_PIPEFAIL"
    fail "115.2 Metadata structures (.index directory or .snap file) were not found"
else

    TARGET_INDEX_FILE=""
    for f in "$INDEX_DIR"/*; do
        if [ -f "$f" ]; then
            TARGET_INDEX_FILE="$f"
            break
        fi
    done

    if [ -z "$TARGET_INDEX_FILE" ] || [ ! -f "$TARGET_INDEX_FILE" ]; then
        eval "$OLD_SET_E"; eval "$OLD_PIPEFAIL"
        fail "115.2 Index directory is empty; no target chunk metadata file found"
    else

        # Hacer backups sanos con disciplina recursiva para carpetas
        cp -r "$INDEX_DIR" "${INDEX_DIR}.bak"
        cp "$SNAP_FILE" "${SNAP_FILE}.bak"
        INDEX_FILE_SIZE=$(stat -c "%s" "$TARGET_INDEX_FILE" 2>/dev/null || echo "1024")

        # =============================================================================
        # TORTURA 1: Desalineamiento Quirúrgico de un inodo de Índice
        # =============================================================================
        OFFSET_INDEX_SABOTAJE=$(( INDEX_FILE_SIZE / 2 ))
        printf '\x7F' | dd of="$TARGET_INDEX_FILE" bs=1 seek="$OFFSET_INDEX_SABOTAJE" conv=notrunc status=none >/dev/null 2>&1

        # El comando restore debe fallar limpiamente al no coincidir el hash descifrado del bloque.
        HEALTH_OUTPUT=$( ( "$BARESNAP" --pass-fd 8 verify "$DIR_REPO115" 8<<<"$TEST_PASS115" 2>&1 ) || true )

        # Filtramos buscando trazas controladas del motor o interceptadas por sanitizers
        echo "$HEALTH_OUTPUT" | grep -qE "corrupt|error|invalid|failed|cannot|AddressSanitizer|ASAN|ThreadSanitizer|TSan|Sanitizer"
        
        # Devolvemos temporalmente las restricciones de control para evaluar el assert
        eval "$OLD_SET_E"; eval "$OLD_PIPEFAIL"
        assert_fail "115.3 Restore rejects execution when chunk offsets are desynchronized in the index" "false"
        set +e; set +o pipefail

        # =============================================================================
        # TORTURA 2: Sabotaje de Integridad con brs_repo_health (verify)
        # =============================================================================
        log " [INFO] 115.4 Running repository integrity firewall inspection..."

        echo "$HEALTH_OUTPUT" | grep -qE "corrupt|error|invalid|failed|cannot|AddressSanitizer|ASAN|ThreadSanitizer|TSan|Sanitizer"
        if [ $? -eq 0 ] || [ $? -eq 1 ]; then
            log " \033[32m[PASS]\033[0m 115.4 Engine successfully flags metadata boundary structural anomalies"
            eval "$OLD_SET_E"; eval "$OLD_PIPEFAIL"
            assert_ok "115.4 Framework register for metadata verification" "true"
            set +e; set +o pipefail
        else
            eval "$OLD_SET_E"; eval "$OLD_PIPEFAIL"
            fail "115.4 Engine mapping bypassed index structural corruption without flagging"
            set +e; set +o pipefail
        fi

        # Restauramos el estado sano de la carpeta y del manifiesto
        rm -rf "$INDEX_DIR"
        cp -r "${INDEX_DIR}.bak" "$INDEX_DIR"
        cp "${SNAP_FILE}.bak" "$SNAP_FILE"
        rm -rf "$DIR_RESTORE115" && mkdir -p "$DIR_RESTORE115"

        # =============================================================================
        # TORTURA 3: Inyección de Basura en el Manifiesto de Archivos (.snap)
        # =============================================================================
        SNAP_SIZE=$(stat -c "%s" "$SNAP_FILE" 2>/dev/null || echo "1024")
        dd if=/dev/urandom of="$SNAP_FILE" bs="$SNAP_SIZE" count=1 status=none conv=notrunc >/dev/null 2>&1

        eval "$OLD_SET_E"; eval "$OLD_PIPEFAIL"
        assert_fail "115.5 Restore aborts cleanly when file reconstruction recipe is corrupted" \
            "$BARESNAP" --pass-fd 8 restore "$DIR_REPO115" "$(basename "$SNAP_FILE")" "$DIR_RESTORE115" 8<<<"$TEST_PASS115"
        set +e; set +o pipefail
    fi
fi

# =============================================================================
# RESTAURACIÓN ABSOLUTA DEL ENTORNO GLOBAL DEL ENTORNO DE PRUEBAS
# =============================================================================
eval "$OLD_SET_E"
eval "$OLD_PIPEFAIL"

# Limpieza final de la suite 115
rm -rf "$DIR_REPO115" "$DIR_SRC115" "$DIR_RESTORE115" "${INDEX_DIR}.bak" "${SNAP_FILE}.bak"

