# ============================================================================
# 113. Crypto: Passphrase Injection via File Descriptor Verification
# ============================================================================
section "113. Crypto: Passphrase Injection via File Descriptor Verification"

DIR_REPO113="$WORK/repo113"
DIR_SRC113="$WORK/src113"
DIR_RESTORE113="$WORK/restore113"
TEST_PASS113="SpartanPass-2026-MetalReal-XChaCha"

# Limpieza absoluta de entornos
rm -rf "$DIR_REPO113" "$DIR_SRC113" "$DIR_RESTORE113"
mkdir -p "$DIR_SRC113" "$DIR_RESTORE113"

# Crear payload de prueba para FastCDC
echo "Metal real e ingenieria de guerrilla sin burocracia v2.4.1" > "$DIR_SRC113/data1.txt"

# 113.1 Inicialización con descriptor de archivo 5 (Here-string)
assert_ok "113.1 Initialize repository using --pass-fd 5" \
    "$BARESNAP" --pass-fd 5 init "$DIR_REPO113" --encrypt 5<<<"$TEST_PASS113"

# 113.2 Creación de Snapshot usando descriptor alternativo (Descriptor 5)
assert_ok "113.2 Create backup snapshot with --pass-fd 5" \
    "$BARESNAP" --pass-fd 5 create "$DIR_REPO113" "$DIR_SRC113" "snap_fd_113" 5<<<"$TEST_PASS113"

# 113.3 Test de Falso Positivo: Intentar verificar con una clave falsa (Debe fallar)
assert_fail "113.3 Verify operation rejects corrupted passphrase cleanly" \
    "$BARESNAP" --pass-fd 5 verify "$DIR_REPO113" 5<<<"ClaveFalsa123"

# Extraer el nombre del snapshot .snap real generado del disco por fecha/hora
SNAP_NAME113=$(ls -1 "$DIR_REPO113/snapshots" 2>/dev/null | grep '\.snap' | head -n 1)

# 113.4 Restauración completa
assert_ok "113.4 Restore snapshot using --pass-fd 5" \
    "$BARESNAP" --pass-fd 5 restore "$DIR_REPO113" "$SNAP_NAME113" "$DIR_RESTORE113" 5<<<"$TEST_PASS113"

# 113.5 Validación matemática binaria bit a bit
log "  [INFO] Executing cross-binary integrity diff..."
diff -r "$DIR_SRC113" "$DIR_RESTORE113/src113" >/dev/null 2>&1
if [ $? -eq 0 ]; then
    log "   \033[32m[PASS]\033[0m 113.5 Payload cross-diff matches perfectly bit by bit"
else
    fail "113.5 Payload cross-diff detected binary corruption"
fi

# Purga de residuos
rm -rf "$DIR_REPO113" "$DIR_SRC113" "$DIR_RESTORE113"
unset TEST_PASS113

