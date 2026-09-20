#!/bin/bash
# ============================================================
# Time-Travel CLI — Test Battery (27 sections)
# Usage: ./mega_test.sh [binary_path]
# ============================================================
set -uo pipefail
exec </dev/null   # avoids hangs from prompts in non-interactive execution
TT="${1:-./timetravel}"
WORK="$(mktemp -d /tmp/tt_battery_XXXX)"
START_EPOCH="$(date +%s)"
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL_COUNT=0

# ---- Automatic section counter ----
SECTION_NUM=0
TOTAL_SECTIONS=$(grep -cE '^[[:space:]]*section[[:space:]]' "$0")
FAILED_TESTS=()
LOGFILE="test_battery.log"
exec > >(tee "$LOGFILE") 2>&1

if [ ! -x "$TT" ]; then
	echo "❌ Binary not found or not executable: $TT"
	echo "   → ./compile.sh"
	exit 1
fi

section() {
	SECTION_NUM=$((SECTION_NUM + 1))
	printf "\n============================================================\n"
	printf "  [%d/%d] %s\n" "$SECTION_NUM" "$TOTAL_SECTIONS" "$1"
	printf "============================================================\n"
}

pass() { PASS_COUNT=$((PASS_COUNT+1)); TOTAL_COUNT=$((TOTAL_COUNT+1)); printf "  ✅ [%03d] %s\n" "$TOTAL_COUNT" "$1"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); TOTAL_COUNT=$((TOTAL_COUNT+1)); printf "  ❌ [%03d] %s\n" "$TOTAL_COUNT" "$1"; FAILED_TESTS+=("$1"); }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); printf "  ⏭  [%03d] %s (SKIP)\n" "$TOTAL_COUNT" "$1"; }

assert_ok()   { local d="$1"; shift; if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi; }
assert_fail() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then fail "$d (should have failed)"; else pass "$d"; fi; }
assert_eq()   { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1 (expected='$2' got='$3')"; fi; }

assert_file_content() {
	if [ ! -f "$2" ]; then fail "$1 (does not exist $2)"; return; fi
	local got; got="$(cat "$2")"
	if [ "$got" = "$3" ]; then pass "$1"; else fail "$1 (expected='$3' got='$got')"; fi
}

assert_contains() {
	if echo "$2" | grep -qF "$3"; then pass "$1"; else fail "$1 (does not contain '$3')"; fi
}

get_record_count() {
	"$TT" status --repo "$1" 2>/dev/null | awk '/Records:/{print $2}'
}

wait_for_stable_records() {
	local dir="$1" prev="" stable=0 cur
	for _ in $(seq 1 60); do
		cur="$(get_record_count "$dir")"
		if [ -n "$cur" ] && [ "$cur" = "$prev" ]; then
			stable=$((stable+1))
			if [ "$stable" -ge 3 ]; then return 0; fi
		else
			stable=0
		fi
		prev="$cur"
		sleep 0.5
	done
	return 0
}

daemon_is_running() {
	local dir="$1"
	[ -f "$dir/.timetravel/timetravel.pid" ] || return 1
	local pid
	pid="$(awk '{print $1}' "$dir/.timetravel/timetravel.pid" 2>/dev/null)"
	[ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# ============================================================
# 1. Setup
# ============================================================
section "1. Setup"
assert_ok "1.1 binary responds to help" "$TT" help

# ============================================================
# 2. basic watch
# ============================================================
section "2. basic watch"
D2="$WORK/t02"; mkdir -p "$D2"
echo "hello" > "$D2/f.txt"
"$TT" start "$D2" >/dev/null 2>&1
sleep 1
if daemon_is_running "$D2"; then pass "2.1 daemon running"; else fail "2.1 daemon is not running"; fi
R2="$(get_record_count "$D2")"
assert_eq "2.2 no initial records" "0" "$R2"
"$TT" stop --repo "$D2" >/dev/null 2>&1

# ============================================================
# 3. undo --last
# ============================================================
section "3. undo --last"
D3="$WORK/t03"; mkdir -p "$D3"
echo "v1" > "$D3/f.txt"
"$TT" start "$D3" >/dev/null 2>&1; sleep 1
echo "v2" > "$D3/f.txt"; sleep 1
"$TT" undo "$D3/f.txt" --last --repo "$D3" >/dev/null 2>&1
assert_file_content "3.1 undo --last restores previous version" "$D3/f.txt" "v1"
"$TT" stop --repo "$D3" >/dev/null 2>&1

# ============================================================
# 4. undo --initial
# ============================================================
section "4. undo --initial"
D4="$WORK/t04"; mkdir -p "$D4"
echo "first" > "$D4/f.txt"
"$TT" start "$D4" >/dev/null 2>&1; sleep 1
echo "second" > "$D4/f.txt"; sleep 1
echo "third" > "$D4/f.txt"; sleep 1
"$TT" undo "$D4/f.txt" --initial --repo "$D4" >/dev/null 2>&1
assert_file_content "4.1 undo --initial restores original" "$D4/f.txt" "first"
"$TT" stop --repo "$D4" >/dev/null 2>&1


# ============================================================
# 5. undo --to (time point & exact log dates)
# ============================================================
# +Added a new expression to 'Undo'; you can now also include
# the log dates, for example: ./timetravel undo src --to "2026-09-13 00:06:17"
section "5. undo --to (time point & exact dates)"
D5="$WORK/t05"; mkdir -p "$D5"

# --- Test relative time ---
echo "v1" > "$D5/f.txt"
"$TT" start "$D5" >/dev/null 2>&1; sleep 1
echo "v2" > "$D5/f.txt"; sleep 2
echo "v3" > "$D5/f.txt"; sleep 1
"$TT" undo "$D5/f.txt" --to "1 seconds ago" --repo "$D5" >/dev/null 2>&1
C5="$(cat "$D5/f.txt")"
if [ "$C5" = "v1" ] || [ "$C5" = "v2" ]; then pass "5.1 undo --to '1 seconds ago' restores past version ($C5)"; else fail "5.1 undo --to gave '$C5'"; fi
assert_fail "5.2 undo --to with invalid expression fails" "$TT" undo "$D5/f.txt" --to "banana" --repo "$D5"

# --- Test exact timestamp (YYYY-MM-DD HH:MM:SS) ---
# Write v_exact_1 and wait for daemon to capture it
echo "v_exact_1" > "$D5/f.txt"; sleep 2

# Write v_exact_2 and wait for daemon to fully process it (debounce + capture)
echo "v_exact_2" > "$D5/f.txt"; sleep 2

# NOW capture the timestamp: guaranteed to be >= daemon's recorded ns for v_exact_2
TS_V2=$(date +"%Y-%m-%d %H:%M:%S")

# Wait to ensure we cross into a new second before writing v_exact_3
sleep 2

# Write v_exact_3: daemon will record this with a timestamp strictly after TS_V2
echo "v_exact_3" > "$D5/f.txt"; sleep 2

"$TT" undo "$D5/f.txt" --to "$TS_V2" --repo "$D5" >/dev/null 2>&1
C5_EXACT="$(cat "$D5/f.txt")"
if [ "$C5_EXACT" = "v_exact_2" ]; then
	pass "5.3 undo --to exact timestamp '$TS_V2' restores correct version"
else
	fail "5.3 undo --to exact timestamp gave '$C5_EXACT' (expected 'v_exact_2')"
fi

"$TT" stop --repo "$D5" >/dev/null 2>&1

# ============================================================
# 6. undo of deleted file
# ============================================================
section "6. undo of deleted file"
D6="$WORK/t06"; mkdir -p "$D6"
echo "content" > "$D6/f.txt"
"$TT" start "$D6" >/dev/null 2>&1; sleep 1
echo "edited" > "$D6/f.txt"; sleep 1
rm "$D6/f.txt"; sleep 1
"$TT" undo "$D6/f.txt" --last --repo "$D6" >/dev/null 2>&1
assert_file_content "6.1 undo restores deleted file" "$D6/f.txt" "edited"
"$TT" stop --repo "$D6" >/dev/null 2>&1

# ============================================================
# 7. undo --to after deletion
# ============================================================
section "7. undo --to after deletion"
D7="$WORK/t07"; mkdir -p "$D7"
echo "orig" > "$D7/f.txt"
"$TT" start "$D7" >/dev/null 2>&1; sleep 1
echo "change" > "$D7/f.txt"; sleep 1
rm "$D7/f.txt"; sleep 1
"$TT" undo "$D7/f.txt" --to "1 seconds ago" --repo "$D7" >/dev/null 2>&1
assert_file_content "7.1 undo --to restores deleted file" "$D7/f.txt" "change"
"$TT" stop --repo "$D7" >/dev/null 2>&1

# ============================================================
# 8. log
# ============================================================
section "8. log"
D8="$WORK/t08"; mkdir -p "$D8"
for i in $(seq 1 30); do echo "line $i with enough content"; done > "$D8/f.txt"
"$TT" start "$D8" >/dev/null 2>&1; sleep 1
echo "new line added at the end" >> "$D8/f.txt"; sleep 1
L8="$("$TT" log "$D8/f.txt" --repo "$D8" 2>&1)"
assert_contains "8.1 log shows CREATE" "$L8" "CREATE"
assert_contains "8.2 log shows MODIFY" "$L8" "MODIFY"
"$TT" stop --repo "$D8" >/dev/null 2>&1

# ============================================================
# 9. status
# ============================================================
section "9. status"
D9="$WORK/t09"; mkdir -p "$D9"
echo "a" > "$D9/f.txt"
"$TT" start "$D9" >/dev/null 2>&1; sleep 1
echo "b" > "$D9/f.txt"
wait_for_stable_records "$D9"
S9="$("$TT" status --repo "$D9" 2>&1)"
assert_contains "9.1 status shows Records" "$S9" "Records:"
assert_contains "9.2 status shows daemon running" "$S9" "running"
"$TT" stop --repo "$D9" >/dev/null 2>&1

# ============================================================
# 10. dynamic: hot new file
# ============================================================
section "10. dynamic: hot new file"
D10="$WORK/t10"; mkdir -p "$D10"
echo "base" > "$D10/base.txt"
"$TT" start "$D10" >/dev/null 2>&1; sleep 1
printf "initial content of the new file\n" > "$D10/new.txt"; sleep 1
printf "initial content of the new file\nmore content\n" > "$D10/new.txt"; sleep 1
"$TT" undo "$D10/new.txt" --last --repo "$D10" >/dev/null 2>&1
assert_file_content "10.1 new file: undo --last restores first version" "$D10/new.txt" "initial content of the new file"
"$TT" stop --repo "$D10" >/dev/null 2>&1

# ============================================================
# 11. dynamic: new subdirectory
# ============================================================
section "11. dynamic: new subdirectory"
D11="$WORK/t11"; mkdir -p "$D11"
echo "r" > "$D11/r.txt"
"$TT" start "$D11" >/dev/null 2>&1; sleep 1
mkdir -p "$D11/new_dir"
echo "deep" > "$D11/new_dir/f.txt"
wait_for_stable_records "$D11"
"$TT" undo "$D11/new_dir/f.txt" --last --repo "$D11" >/dev/null 2>&1
if [ -f "$D11/new_dir/f.txt" ]; then pass "11.1 file in new subdirectory restorable"; else fail "11.1 new subdirectory was not registered"; fi
"$TT" stop --repo "$D11" >/dev/null 2>&1

# ============================================================
# 12. dynamism after restart
# ============================================================
section "12. dynamism after restart"
D12="$WORK/t12"; mkdir -p "$D12"
echo "a" > "$D12/f.txt"
"$TT" start "$D12" >/dev/null 2>&1; sleep 1
"$TT" stop --repo "$D12" >/dev/null 2>&1; sleep 1
"$TT" start "$D12" >/dev/null 2>&1; sleep 1
echo "b" > "$D12/f.txt"; sleep 1
"$TT" undo "$D12/f.txt" --last --repo "$D12" >/dev/null 2>&1
assert_file_content "12.1 undo --last after restart" "$D12/f.txt" "a"
"$TT" stop --repo "$D12" >/dev/null 2>&1

# ============================================================
# 13. Git clone
# ============================================================
section "13. Git clone"
if command -v git >/dev/null 2>&1; then
	GO="$WORK/git_origin"
	git init -q "$GO" 2>/dev/null
	git -C "$GO" config user.email "t@t.t" 2>/dev/null
	git -C "$GO" config user.name "t" 2>/dev/null
	for i in 1 2 3; do echo "file $i" > "$GO/f$i.txt"; done
	git -C "$GO" add -A >/dev/null 2>&1
	git -C "$GO" commit -qm "init" 2>/dev/null
	D13="$WORK/t13"; mkdir -p "$D13"
	echo "base" > "$D13/base.txt"
	"$TT" start "$D13" >/dev/null 2>&1; sleep 1
	git clone -q "$GO" "$D13/repo" 2>/dev/null
	wait_for_stable_records "$D13"
	if [ -f "$D13/repo/f1.txt" ]; then pass "13.1 clone present"; else fail "13.1 clone not present"; fi
	H13="$("$TT" log "" --repo "$D13" 2>&1)"
	if echo "$H13" | grep -q "\.git/"; then fail "13.2 .git/ was captured"; else pass "13.2 .git/ excluded"; fi
	echo "change" > "$D13/repo/f1.txt"; sleep 1
	"$TT" undo "$D13/repo/f1.txt" --last --repo "$D13" >/dev/null 2>&1
	assert_file_content "13.3 undo of clone file" "$D13/repo/f1.txt" "file 1"
	"$TT" stop --repo "$D13" >/dev/null 2>&1
else
	skip "13.x git not installed"
fi

# ============================================================
# 14. Binary file
# ============================================================
section "14. Binary file"
D14="$WORK/t14"; mkdir -p "$D14"
dd if=/dev/urandom of="$D14/b.bin" bs=1024 count=64 2>/dev/null
cp "$D14/b.bin" "$D14/b.orig"
"$TT" start "$D14" >/dev/null 2>&1; sleep 1
dd if=/dev/urandom of="$D14/b.bin" bs=1024 count=64 2>/dev/null
sleep 1
"$TT" undo "$D14/b.bin" --last --repo "$D14" >/dev/null 2>&1
if cmp -s "$D14/b.bin" "$D14/b.orig"; then pass "14.1 binary restored byte by byte"; else fail "14.1 binary differs"; fi
"$TT" stop --repo "$D14" >/dev/null 2>&1

# ============================================================
# 15. Empty file
# ============================================================
section "15. Empty file"
D15="$WORK/t15"; mkdir -p "$D15"
echo "content" > "$D15/f.txt"
"$TT" start "$D15" >/dev/null 2>&1; sleep 1
: > "$D15/f.txt"; sleep 1
"$TT" undo "$D15/f.txt" --last --repo "$D15" >/dev/null 2>&1
assert_file_content "15.1 undo restores after truncation to empty" "$D15/f.txt" "content"
"$TT" stop --repo "$D15" >/dev/null 2>&1

# ============================================================
# 16. compact
# ============================================================
section "16. compact"
D16="$WORK/t16"; mkdir -p "$D16"
echo "v0" > "$D16/f.txt"
"$TT" start "$D16" >/dev/null 2>&1; sleep 1
for i in $(seq 1 20); do echo "version $i with content" > "$D16/f.txt"; sleep 0.3; done
wait_for_stable_records "$D16"
"$TT" stop --repo "$D16" >/dev/null 2>&1
assert_ok "16.1 compact executes" "$TT" compact --repo "$D16"
"$TT" start "$D16" >/dev/null 2>&1; sleep 1
echo "vfinal" > "$D16/f.txt"; sleep 1
"$TT" undo "$D16/f.txt" --last --repo "$D16" >/dev/null 2>&1
C16="$(cat "$D16/f.txt")"
if [ "$C16" = "vfinal" ]; then fail "16.2 undo after compact did not restore"; else pass "16.2 undo after compact works ($C16)"; fi
"$TT" stop --repo "$D16" >/dev/null 2>&1

# ============================================================
# 17. full directory undo
# ============================================================
section "17. full directory undo"
D17="$WORK/t17"; mkdir -p "$D17/sub"
echo "a1" > "$D17/a.txt"; echo "b1" > "$D17/b.txt"; echo "c1" > "$D17/sub/c.txt"
"$TT" start "$D17" >/dev/null 2>&1; sleep 1
echo "a2" > "$D17/a.txt"; echo "b2" > "$D17/b.txt"; echo "c2" > "$D17/sub/c.txt"
wait_for_stable_records "$D17"
"$TT" undo "$D17" --last --repo "$D17" --force >/dev/null 2>&1
assert_file_content "17.1 undo dir --last restores a.txt" "$D17/a.txt" "a1"
assert_file_content "17.2 undo dir --last restores sub/c.txt" "$D17/sub/c.txt" "c1"
"$TT" stop --repo "$D17" >/dev/null 2>&1

# ============================================================
# 18. Burst: 50 edits to same file
# ============================================================
section "18. Burst: 50 edits"
D18="$WORK/t18"; mkdir -p "$D18"
echo "v0" > "$D18/f.txt"
"$TT" start "$D18" >/dev/null 2>&1; sleep 1
for i in $(seq 1 50); do echo "burst $i" > "$D18/f.txt"; done
wait_for_stable_records "$D18"
"$TT" undo "$D18/f.txt" --initial --repo "$D18" >/dev/null 2>&1
assert_file_content "18.1 undo --initial after burst" "$D18/f.txt" "v0"
"$TT" stop --repo "$D18" >/dev/null 2>&1

# ============================================================
# 19. Burst: 100 files + concurrency
# ============================================================
section "19. Burst: 100 files + concurrency"
D19="$WORK/t19"; mkdir -p "$D19"
for i in 1 2 3 4 5; do echo "init $i" > "$D19/w$i.txt"; done
"$TT" start "$D19" >/dev/null 2>&1; sleep 1
for i in $(seq 1 100); do echo "f $i" > "$D19/r$i.txt"; done
WPIDS=""
for i in 1 2 3 4 5; do
	( for j in $(seq 1 20); do printf "writer %d iter %d\n" "$i" "$j" > "$D19/w$i.txt"; sleep 0.01; done ) &
	WPIDS="$WPIDS $!"
done
wait $WPIDS
wait_for_stable_records "$D19"
"$TT" undo "$D19/w3.txt" --last --repo "$D19" >/dev/null 2>&1
C19="$(cat "$D19/w3.txt")"
if echo "$C19" | grep -qE "writer 3|init 3"; then pass "19.1 undo --last after concurrency coherent"; else fail "19.1 undo after concurrency gave '$C19'"; fi
"$TT" stop --repo "$D19" >/dev/null 2>&1

# ============================================================
# 20. Stress: 500 files
# ============================================================
section "20. Stress: 500 files"
D20="$WORK/t20"; mkdir -p "$D20"
"$TT" start "$D20" >/dev/null 2>&1; sleep 1
for i in $(seq 1 500); do printf "content of file %d - %s\n" "$i" "$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')" > "$D20/stress_$i.txt"; done
wait_for_stable_records "$D20"
R20="$(get_record_count "$D20")"
if [ "${R20:-0}" -ge 100 ] 2>/dev/null; then pass "20.1 500 files captured ($R20)"; else fail "20.1 only $R20 records"; fi
for i in 1 100 250 500; do echo "modified $i" > "$D20/stress_$i.txt"; done
wait_for_stable_records "$D20"
"$TT" undo "$D20/stress_250.txt" --last --repo "$D20" >/dev/null 2>&1
C20="$(cat "$D20/stress_250.txt")"
if echo "$C20" | grep -q "content of file 250"; then pass "20.2 undo --last on file 250/500"; else fail "20.2 undo --last on file 250/500 gave: '$(echo "$C20" | head -1)'"; fi
"$TT" stop --repo "$D20" >/dev/null 2>&1

# ============================================================
# 21. Files with weird names
# ============================================================
section "21. Weird names"
D21="$WORK/t21"; mkdir -p "$D21"
echo "a" > "$D17/x" 2>/dev/null
echo "v1" > "$D21/with spaces.txt"
echo "v1" > "$D21/accents_ñ.txt"
"$TT" start "$D21" >/dev/null 2>&1; sleep 1
echo "v2" > "$D21/with spaces.txt"
echo "v2" > "$D21/accents_ñ.txt"
wait_for_stable_records "$D21"
"$TT" undo "$D21/with spaces.txt" --last --repo "$D21" >/dev/null 2>&1
assert_file_content "21.1 name with spaces" "$D21/with spaces.txt" "v1"
"$TT" undo "$D21/accents_ñ.txt" --last --repo "$D21" >/dev/null 2>&1
assert_file_content "21.2 name with ñ" "$D21/accents_ñ.txt" "v1"
"$TT" stop --repo "$D21" >/dev/null 2>&1

# ============================================================
# 22. start / stop / restart
# ============================================================
section "22. start / stop / restart"
D22="$WORK/t22"; mkdir -p "$D22"
echo "v1" > "$D22/f.txt"
assert_ok "22.1 start launches daemon" "$TT" start "$D22"
sleep 1
if daemon_is_running "$D22"; then pass "22.2 daemon running after start"; else fail "22.2 daemon is not running"; fi
echo "v2" > "$D22/f.txt"; sleep 1
assert_ok "22.3 duplicate start does not launch another daemon" "$TT" start "$D22"
assert_ok "22.4 stop stops the daemon" "$TT" stop --repo "$D22"
sleep 1
if daemon_is_running "$D22"; then fail "22.5 daemon still alive after stop"; else pass "22.5 daemon stopped after stop"; fi
assert_ok "22.6 restart starts again" "$TT" restart "$D22"
sleep 1
if daemon_is_running "$D22"; then pass "22.7 daemon running after restart"; else fail "22.7 daemon is not running after restart"; fi
"$TT" stop --repo "$D22" >/dev/null 2>&1

# ============================================================
# 23. enriched status
# ============================================================
section "23. enriched status"
D23="$WORK/t23"; mkdir -p "$D23"
echo "a" > "$D23/f.txt"
"$TT" start "$D23" >/dev/null 2>&1; sleep 1
echo "b" > "$D23/f.txt"
wait_for_stable_records "$D23"
S23="$("$TT" status --repo "$D23" 2>&1)"
assert_contains "23.1 status shows daemon running" "$S23" "running"
assert_contains "23.2 status shows Records" "$S23" "Records:"
assert_contains "23.3 status shows Uptime" "$S23" "Uptime:"
"$TT" stop --repo "$D23" >/dev/null 2>&1
sleep 1
S23B="$("$TT" status --repo "$D23" 2>&1)"
assert_contains "23.4 status without daemon indicates not running" "$S23B" "not running"

# ============================================================
# 24. tags and undo --tag
# ============================================================
section "24. tags and undo --tag"
D24="$WORK/t24"; mkdir -p "$D24"
echo "stable" > "$D24/f.txt"
"$TT" start "$D24" >/dev/null 2>&1; sleep 1
assert_ok "24.1 tag is created" "$TT" tag checkpoint1 --repo "$D24"
T24="$("$TT" tags --repo "$D24" 2>&1)"
assert_contains "24.2 tags lists the checkpoint" "$T24" "checkpoint1"
echo "changed" > "$D24/f.txt"; sleep 1
"$TT" undo "$D24/f.txt" --tag checkpoint1 --repo "$D24" >/dev/null 2>&1
assert_file_content "24.3 undo --tag restores to checkpoint" "$D24/f.txt" "stable"
"$TT" stop --repo "$D24" >/dev/null 2>&1

# ============================================================
# 25. diff
# ============================================================
section "25. diff"
D25="$WORK/t25"; mkdir -p "$D25"
printf "line1\nline2\n" > "$D25/f.txt"
"$TT" start "$D25" >/dev/null 2>&1; sleep 1
printf "line1\nline2\nline3\n" > "$D25/f.txt"
wait_for_stable_records "$D25"
DF25="$("$TT" diff "$D25/f.txt" --repo "$D25" 2>&1)"
if echo "$DF25" | grep -qE "line3|\+"; then pass "25.1 diff shows the change"; else fail "25.1 diff does not show the change"; fi
"$TT" stop --repo "$D25" >/dev/null 2>&1

# ============================================================
# 26. directory undo: confirmation and --force
# ============================================================
section "26. directory undo: confirmation and --force"
D26="$WORK/t26"; mkdir -p "$D26"
echo "orig" > "$D26/a.txt"
"$TT" start "$D26" >/dev/null 2>&1; sleep 1
echo "new" > "$D26/a.txt"
wait_for_stable_records "$D26"
echo "n" | "$TT" undo "$D26" --last --repo "$D26" >/dev/null 2>&1
assert_file_content "26.1 undo dir without confirmation touches nothing" "$D26/a.txt" "new"
"$TT" undo "$D26" --last --repo "$D26" --force >/dev/null 2>&1
assert_file_content "26.2 undo dir --force restores" "$D26/a.txt" "orig"
"$TT" stop --repo "$D26" >/dev/null 2>&1

# ============================================================
# 27. log --since
# ============================================================
section "27. log --since"
D27="$WORK/t27"; mkdir -p "$D27"
echo "x" > "$D27/f.txt"
"$TT" start "$D27" >/dev/null 2>&1; sleep 1
echo "y" > "$D27/f.txt"
wait_for_stable_records "$D27"
L27="$("$TT" log "$D27/f.txt" --since "10 minutes ago" --repo "$D27" 2>&1)"
assert_contains "27.1 log --since shows recent events" "$L27" "f.txt"
L27B="$("$TT" log "$D27/f.txt" --since "1 hours ago" --repo "$D27" 2>&1)"
assert_contains "27.2 log --since wide range includes everything" "$L27B" "f.txt"
"$TT" stop --repo "$D27" >/dev/null 2>&1

# ============================================================
# Summary
# ============================================================
END_EPOCH="$(date +%s)"
ELAPSED=$((END_EPOCH - START_EPOCH))
printf "\n============================================================\n"
printf "  SUMMARY\n"
printf "============================================================\n"
printf "  Passed:    %d/%d\n" "$PASS_COUNT" "$TOTAL_COUNT"
printf "  Failed:    %d\n" "$FAIL_COUNT"
printf "  Skipped:   %d\n" "$SKIP_COUNT"
printf "  Duration:  %dm %02ds\n" $((ELAPSED / 60)) $((ELAPSED % 60))
printf "  Log:       %s\n" "$LOGFILE"
printf "============================================================\n"

if [ "$FAIL_COUNT" -gt 0 ]; then
	printf "\n💥 FAILED TESTS (%d):\n" "$FAIL_COUNT"
	for f in "${FAILED_TESTS[@]}"; do printf "  ✗ %s\n" "$f"; done
	printf "\n"
	exit 1
fi

printf "\n🎉 ALL TESTS PASSED in %dm %02ds\n" $((ELAPSED / 60)) $((ELAPSED % 60))
exit 0
