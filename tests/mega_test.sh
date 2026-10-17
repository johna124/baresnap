#!/usr/bin/env bash

set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
export MEGA_REFTEST_HOME="$SCRIPT_DIR/"
export MEGA_SUITE_DIR="$MEGA_REFTEST_HOME/suites"

if [ ! -d "$MEGA_SUITE_DIR" ]; then
    exit 1
fi

source "$MEGA_REFTEST_HOME/lib/00_bootstrap.sh" "$@"

if [ -f "$MEGA_REFTEST_HOME/lib/50_helpers.sh" ]; then
    source "$MEGA_REFTEST_HOME/lib/50_helpers.sh"
fi

for suite in "$MEGA_SUITE_DIR"/*.sh; do
    if [ -f "$suite" ]; then
        
        # ========================================================================
        # INTER-TEST ANTICROSS-CONTAMINATION PURGE VALVE (Safe for Chain Tests)
        # ========================================================================
        # 1. Limpiamos los temporales puros y rastros de red del sistema en /tmp
        rm -rf /tmp/baresnap_tests.* 2>/dev/null || true
        rm -rf /tmp/baresnap_sat_repo* 2>/dev/null || true
        rm -rf /tmp/baresnap_mega_test_repo* 2>/dev/null || true
        rm -rf /tmp/trace*.txt 2>/dev/null || true
        
        # 2. Evitamos que las contraseñas criptográficas se queden flotando
        unset BARESNAP_PASSPHRASE
        unset BARESNAP_KEY_FILE
        
        # 3. Matamos procesos remotos o sockets de red zombis que bloqueen memoria
        pkill -9 baresnap 2>/dev/null || true
        pkill -9 baresnap-remote 2>/dev/null || true
        
        # 4. 🚯 ELIMINADO: Ya NO borramos "$WORK"/* para preservar los repositorios 
        # persistentes que los tests siguientes necesitan heredar del disco.
        # ========================================================================

        source "$suite"
    fi
done


if [ -f "$MEGA_REFTEST_HOME/lib/99_summary.sh" ]; then
    source "$MEGA_REFTEST_HOME/lib/99_summary.sh"
fi

