# ============================================================
# 73. Induced Bit Rot: Cosmic Ray Simulation in Packs (AES-256-GCM)
# ============================================================
section "73. Induced Bit Rot: Cosmic Ray in Packs (AES-256-GCM)"
REPO73="$WORK/repo73"
SRC73="$WORK/src73"
OUT73="$WORK/out73"
rm -rf "$REPO73" "$SRC73" "$OUT73"
mkdir -p "$SRC73"
export BARESNAP_PASSPHRASE="ClaveSecretaRayoCosmico"
echo "High fidelity payload that will suffer bit rot" > "$SRC73/target.txt"
head -c 8192 /dev/urandom > "$SRC73/padding.bin"
AES73_OK=1
if ! assert_init_aes "73.1 init --encrypt aes" \
"$BARESNAP" init "$REPO73" --encrypt aes; then
AES73_OK=0
fi
if [ "$AES73_OK" -eq 1 ] && ! assert_info_aes "73.1b info confirms AES" \
"$BARESNAP" info "$REPO73"; then
AES73_OK=0
fi
if [ "$AES73_OK" -eq 1 ]; then
assert_ok "73.1c create snapshot pre-rot" \
"$BARESNAP" create "$REPO73" "$SRC73" "snap_pre_rot"
# Locate the physical pack and mutate 1 byte
PACK73=$(find "$REPO73/packs" -type f -name '*.pack' 2>/dev/null | head -n 1)
if [ -n "$PACK73" ] && [ -f "$PACK73" ]; then
PACK73_SIZE=$(stat -c '%s' "$PACK73")
if [ "$PACK73_SIZE" -gt 2048 ]; then
ROT73_OFF=1024
else
ROT73_OFF=$((PACK73_SIZE / 2))
fi
printf '\xFF' | dd of="$PACK73" bs=1 seek="$ROT73_OFF" conv=notrunc status=none 2>/dev/null
pass "73.2 injection of 1 byte mutation at offset $ROT73_OFF"
else
fail "73.2 no physical pack found to corrupt"
fi
# verify must catch the alteration of the GCM/MAC Tag
set +e
"$BARESNAP" verify "$REPO73" >/dev/null 2>&1
RC73_V=$?
set +e
if [ "$RC73_V" -ne 0 ]; then
pass "73.3 verify detected integrity failure (altered MAC/GCM Tag)"
else
fail "73.3 verify did not detect the bit rot corruption"
fi
# restore must abort atomically (Avoid SIGSEGV)
set +e
"$BARESNAP" restore "$REPO73" "snap_pre_rot" "$OUT73" >/dev/null 2>&1
RC73_R=$?
set +e
if [ "$RC73_R" -ge 128 ]; then
fail "73.4 restore CRASHED with signal $((RC73_R - 128)) (memory panic)"
elif [ "$RC73_R" -ne 0 ]; then
pass "73.4 automatic pack quarantine without memory panic"
else
fail "73.4 the engine allowed restoring data corrupted by bit rot"
fi
else
fail "73.1c create snapshot pre-rot (skip due to init/info AES failure)"
fail "73.2 injection of 1 byte mutation (skip)"
fail "73.3 verify detected integrity failure (skip)"
fail "73.4 automatic pack quarantine (skip)"
fi
unset BARESNAP_PASSPHRASE

