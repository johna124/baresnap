# ============================================================
# 64. Index Checksum: FNV1A-64 / BLAKE2B integrity
# ============================================================
section "64. Index Checksum: FNV1A-64 / BLAKE2B integrity"
REPO64="$WORK/repo64"
SRC64="$WORK/src64"
assert_ok "64.1 init" "$BARESNAP" init "$REPO64"
mkdir -p "$SRC64"
printf 'checksum test data
' > "$SRC64/data.txt"
head -c 65536 /dev/urandom > "$SRC64/blob.bin"
assert_ok "64.2 create" "$BARESNAP" create "$REPO64" "$SRC64"
assert_ok "64.3 verify before corrupting" "$BARESNAP" verify "$REPO64"
IDX_FILE=$(ls "$REPO64/index/"*.idx 2>/dev/null | head -1)
IDX_BACKUP="$WORK/idx64_backup.idx"

if [ -n "$IDX_FILE" ]; then
    cp "$IDX_FILE" "$IDX_BACKUP"
    IDX_SIZE=$(stat -c '%s' "$IDX_FILE")
    
    # Adaptamos la longitud de la cola según el modo de hash activo del Megatest
    if [ "$HASH_MODE" = "blake2b" ]; then
        TAIL_BYTES=16 # Blake2b_128 usa 16 bytes finales de control
        EXPECTED_ZERO="00000000000000000000000000000000"
    else
        TAIL_BYTES=8  # FNV1A-64 usa 8 bytes tradicionales
        EXPECTED_ZERO="0000000000000000"
    fi

    if [ "$IDX_SIZE" -gt $TAIL_BYTES ]; then
        CKSUM_HEX=$(tail -c $TAIL_BYTES "$IDX_FILE" | xxd -p | tr -d ' \n')
        if [ "$CKSUM_HEX" != "$EXPECTED_ZERO" ]; then
            pass "64.4 index checksum is non-zero ($HASH_MODE): $CKSUM_HEX"
        else
            fail "64.4 index checksum is zero (not implemented)"
        fi
    else
        fail "64.4 index too small to have checksum"
    fi

    # Forzamos que la corrupción se inyecte de forma segura sin romper la alineación binaria de C
    CORRUPT_OFFSET=$((IDX_SIZE - TAIL_BYTES - 4)) # Corrompemos la zona de registros, nunca el layout criptográfico
    printf '\xFF' | dd of="$IDX_FILE" bs=1 seek="$CORRUPT_OFFSET" conv=notrunc status=none 2>/dev/null
    
    set +e
    "$BARESNAP" verify "$REPO64" >/dev/null 2>&1
    VERIFY_RC64=$?
    set -e
        if [ "$VERIFY_RC64" -ne 0 ]; then
        pass "64.5 verify detects corrupt index via checksum (rc=$VERIFY_RC64)"
    else
        fail "64.5 verify did NOT detect corrupt index"
    fi
    
    cp "$IDX_BACKUP" "$IDX_FILE"
    assert_ok "64.6 verify passes after restoring clean index" "$BARESNAP" verify "$REPO64"
else
    fail "64.4 no index found to corrupt"
    fail "64.5 skip"
    fail "64.6 skip"
fi
  

