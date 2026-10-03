# ============================================================
# 06. File cache in incrementals
# ============================================================
section "06. File cache in incrementals"
REPO="$WORK/repo06"
SRC="$WORK/src06"
assert_ok "init" "$BARESNAP" init "$REPO"
mkdir -p "$SRC"
printf 'stable file
' > "$SRC/stable.txt"
head -c 65536 /dev/urandom > "$SRC/blob.bin"
assert_ok "create 1" "$BARESNAP" create "$REPO" "$SRC"
sleep 1.1
OUT1=$("$BARESNAP" create "$REPO" "$SRC" 2>&1)
HITS1=$(printf '%s
' "$OUT1" | grep -oE '[0-9]+ hits' | grep -oE '[0-9]+' | head -1)
MISS1=$(printf '%s
' "$OUT1" | grep -oE '[0-9]+ misses' | grep -oE '[0-9]+' | head -1)
assert_eq "create 2: cache hits" "2" "$HITS1"
assert_eq "create 2: cache misses" "0" "$MISS1"
printf 'modified
' > "$SRC/stable.txt"
OUT2=$("$BARESNAP" create "$REPO" "$SRC" 2>&1)
HITS2=$(printf '%s
' "$OUT2" | grep -oE '[0-9]+ hits' | grep -oE '[0-9]+' | head -1)
MISS2=$(printf '%s
' "$OUT2" | grep -oE '[0-9]+ misses' | grep -oE '[0-9]+' | head -1)
assert_eq "create 3 (1 mod): cache hits" "1" "$HITS2"
assert_eq "create 3 (1 mod): cache misses" "1" "$MISS2"

