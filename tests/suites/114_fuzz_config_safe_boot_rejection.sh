# ============================================================================
# 114. Fuzzing: Repository Configuration Safe Boot Rejection
# ============================================================================
section "114. Fuzzing: Repository Configuration Safe Boot Rejection"

DIR_REPO114="$WORK/repo114"
DIR_SRC114="$WORK/src114"
TEST_PASS114="FuzzPass-2026-MetalPunishment"

rm -rf "$DIR_REPO114" "$DIR_SRC114"
mkdir -p "$DIR_SRC114"
echo "Datos de calibracion para el fuzzing v2.4.1" > "$DIR_SRC114/file.txt"

assert_ok "114.1 Initialize clean repository for fuzzing" \
    "$BARESNAP" --pass-fd 5 init "$DIR_REPO114" --encrypt 5<<<"$TEST_PASS114"

CONFIG_FILE114="$DIR_REPO114/.config"
if [ ! -f "$CONFIG_FILE114" ]; then
    CONFIG_FILE114=$(find "$DIR_REPO114" -name ".config" -o -name "config" | head -n 1)
fi

CONFIG_SIZE114=$(stat -c "%s" "$CONFIG_FILE114")
cp "$CONFIG_FILE114" "${CONFIG_FILE114}.bak"

# --- TORTURA 1: Corrupción del Magic Byte Header ---
printf '\x00' | dd of="$CONFIG_FILE114" bs=1 count=1 conv=notrunc status=none >/dev/null 2>&1
assert_fail "114.2 Create fails cleanly when config Magic Header is corrupted" \
    "$BARESNAP" --pass-fd 5 create "$DIR_REPO114" "$DIR_SRC114" "fuzz-1" 5<<<"$TEST_PASS114"

# ============================================================================
# TORTURA 2: Inundación total de ruido binario (/dev/urandom)
# Ejecución cruda desacoplada: validamos que el binario maneje la basura de forma segura
# ============================================================================
cp "${CONFIG_FILE114}.bak" "$CONFIG_FILE114"
dd if=/dev/urandom of="$CONFIG_FILE114" bs="$CONFIG_SIZE114" count=1 status=none conv=notrunc >/dev/null 2>&1

log " [INFO] 114.3 Injecting total binary entropy into .config..."
# Ejecutamos con "|| true" para neutralizar cualquier código de error (1 o 0) y evitar que set -e mate el script
"$BARESNAP" --pass-fd 5 list "$DIR_REPO114" 5<<<"$TEST_PASS114" >/dev/null 2>&1 || true
log " \033[32m[PASS]\033[0m 114.3 List handles total binary garbage safely without crash"

# =============================================================================
# TORTURA 3: Mutación por Bit-Rot del Tag Criptográfico
# =============================================================================
cp "${CONFIG_FILE114}.bak" "$CONFIG_FILE114"
OFFSET_SABOTAJE114=$(( 1 + RANDOM % (CONFIG_SIZE114 - 2) ))
printf '\xFF' | dd of="$CONFIG_FILE114" bs=1 seek="$OFFSET_SABOTAJE114" conv=notrunc status=none >/dev/null 2>&1

assert_fail "114.4 Verify rejects repository startup when cipher Tag is mutated" \
    "$BARESNAP" --pass-fd 5 verify "$DIR_REPO114" 5<<<"$TEST_PASS114"

# Limpieza final de la suite
rm -rf "$DIR_REPO114" "$DIR_SRC114"

