# ============================================================================
# 11. Info (Autónomo y Blindado contra Contaminación)
# ============================================================================
section "11. Info"

# 1. Definimos una ruta única y limpia para este test
REPO11="$WORK/repo11_info"
SRC11="$WORK/src11_info"

rm -rf "$REPO11" "$SRC11"
mkdir -p "$SRC11"

# 2. Creamos un dataset mínimo para generar estadísticas reales en las secciones
printf 'test info content\n' > "$SRC11/info_data.txt"

# 3. Inicializamos y creamos un snapshot de respaldo nativo sin cifrar
assert_ok "info preparation: init" "$BARESNAP" init "$REPO11"
assert_ok "info preparation: create" "$BARESNAP" create "$REPO11" "$SRC11"

# 4. Ahora ejecutamos el comando info real sobre nuestro repositorio garantizado
INFO_OUT=$("$BARESNAP" info "$REPO11" 2>&1)
INFO_RC=$?

assert_eq "info exit code" "0" "$INFO_RC"

# 5. Validamos que el volcado de la TUI contenga todas las secciones obligatorias
for section_name in Repository Snapshots Packs Chunks Storage Index Cache; do
    if printf '%s\n' "$INFO_OUT" | grep -q "$section_name"; then
        pass "info shows section: $section_name"
    else
        fail "info does not show section: $section_name"
    fi
done

# Limpieza de cortesía al terminar
rm -rf "$REPO11" "$SRC11"

