#!/usr/bin/env bash
# ============================================================
# baresnap_gui.sh — BareSnap GUI with yad (v7.0 - Multi-Crypto AES-256-GCM)
# ============================================================
BARESNAP="${BARESNAP:-./baresnap}"
TITLE="BareSnap"
# --- Check dependencies ---
if ! command -v yad &>/dev/null; then
zenity --error --text="yad is not installed.
Run: sudo apt install yad" 2>/dev/null ||
echo "ERROR: yad not installed. Run: sudo apt install yad"
exit 1
fi
if [ ! -x "$BARESNAP" ]; then
yad --error --title="$TITLE" --text="Cannot find binary: $BARESNAP
Compile first: ./compile.sh"
exit 1
fi
# --- Helpers ---
show_output() {
local title="$1" output="$2" rc="$3" cmd="$4"
local status="OK"
[ "$rc" -ne 0 ] && status="ERROR (code $rc)"
yad --text-info --title="$TITLE - $title" \
--width=750 --height=500 \
--button="Close" \
<<< "Status: $status
Command: $cmd
$output"
}
# --- NORMAL EXECUTION (Without progress bar) ---
run_baresnap() {
local title="$1"
shift
local output rc
output=$("$BARESNAP" "$@" 2>&1)
rc=$?
if [ $rc -ne 0 ]; then
show_output "$title" "$output" "$rc" "$BARESNAP $*"
else
local display_output="$output"
local lines
lines=$(echo "$output" | wc -l)
if [ "$lines" -gt 25 ]; then
display_output=$(echo "$output" | tail -n 25)
display_output="... (summary of the last 25 lines) ...
$display_output"
fi
yad --info --title="$TITLE - $title" \
--text="Operation completed successfully.
$display_output" \
--width=650 --height=450 \
--button="Close:0"
fi
return $rc
}
# --- EXECUTION WITH PROGRESS BAR (FIFO Anti-Deadlock) ---
run_with_progress() {
local title="$1"
shift
local pipe="/tmp/brs_progress_$$"
rm -f "$pipe"
mkfifo "$pipe" 2>/dev/null || true
exec 3<>"$pipe"
yad --progress --title="$TITLE - $title" \
--text="Processing..." --auto-close --no-buttons \
--percentage=0 <&3 &
local yad_pid=$!
export BARESNAP_PROGRESS_PIPE="$pipe"
local output rc
output=$("$BARESNAP" "$@" 2>&1)
rc=$?
unset BARESNAP_PROGRESS_PIPE
echo "100" >&3 2>/dev/null
exec 3>&- 2>/dev/null
wait $yad_pid 2>/dev/null
rm -f "$pipe"
if [ $rc -ne 0 ]; then
show_output "$title" "$output" "$rc" "$BARESNAP $*"
else
local summary
summary=$(echo "$output" | tail -n 20)
yad --info --title="$TITLE - $title" \
--text="Operation completed successfully.
Summary:
$summary" \
--width=650 --height=450 \
--button="Close:0"
fi
return $rc
}
ask_repo() {
yad --file --directory --title="$TITLE - Select or create repository" --width=600 --height=400
}
ask_snap() {
local repo="$1"
local snapdir="$repo/snapshots"
if [ -d "$snapdir" ]; then
yad --file --title="$TITLE - Select snapshot" \
--file-filter="Snapshots | *.snap" \
--filename="$snapdir/" --width=600 --height=400
else
yad --file --title="$TITLE - Select snapshot" \
--file-filter="Snapshots | *.snap" --width=600 --height=400
fi
}
ask_dir() {
local title="$1"
yad --file --directory --title="$TITLE - $title" --width=600 --height=400
}
# ============================================================
# Operations
# ============================================================
# --- INITIALIZE REPOSITORY (NEW: multi-crypto + compression) ---
do_init() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
if [ -f "$repo/config" ]; then
yad --info --title="$TITLE" \
--text="The directory '$repo' is already a valid repository."
return
fi
local opts
opts=$(yad --form --title="$TITLE - Initialize Repository" \
--text="Repo: $repo" \
--field="Encrypt repository:CHK" "FALSE" \
--field="Encryption algorithm:CB" "chacha20!aes256gcm" \
--field="Compression:CB" "lz4!zstd" \
--field="ZSTD Level (1-22):NUM" "3!1..22" \
--field="Passphrase (if encrypting):H" "" \
--width=520 --height=300)
[ -z "$opts" ] && return
local do_encrypt cipher comp zlvl pass
do_encrypt=$(echo "$opts" | cut -d'|' -f1)
cipher=$(echo "$opts" | cut -d'|' -f2)
comp=$(echo "$opts" | cut -d'|' -f3)
zlvl=$(echo "$opts" | cut -d'|' -f4)
pass=$(echo "$opts" | cut -d'|' -f5)
local args=()
if [ "$do_encrypt" = "TRUE" ]; then
if [ -z "$pass" ]; then
yad --error --title="$TITLE" --text="Empty passphrase."
return
fi
export BARESNAP_PASSPHRASE="$pass"
if [ "$cipher" = "aes256gcm" ] || [ "$cipher" = "aes" ]; then
args+=(--encrypt aes)
else
args+=(--encrypt)
fi
fi
if [ "$comp" = "zstd" ]; then
args+=(--compression zstd --zstd-level "$zlvl")
fi
local output rc
output=$("$BARESNAP" init "$repo" "${args[@]}" 2>&1)
rc=$?
unset BARESNAP_PASSPHRASE
show_output "Initialize Repository" "$output" $rc "$BARESNAP init $repo ${args[*]}"
}
# --- CREATE BACKUP (WITH AUTO-INIT AND ENCRYPTION SELECTOR) ---
do_create() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
# 1. SILENT AUTO-INIT (Only interrupts if there is an error)
if [ ! -f "$repo/config" ]; then
local init_opts
init_opts=$(yad --form --title="$TITLE - Initialize Repository" \
--text="The directory '$repo' is not a valid repository.
It will be initialized automatically." \
--field="Encrypt repository:CHK" "FALSE" \
--field="Encryption algorithm:CB" "chacha20!aes256gcm" \
--field="Passphrase (if encrypting):H" "" \
--width=450 --height=250)
[ -z "$init_opts" ] && return
local do_encrypt cipher_algo init_pass
do_encrypt=$(echo "$init_opts" | cut -d'|' -f1)
cipher_algo=$(echo "$init_opts" | cut -d'|' -f2)
init_pass=$(echo "$init_opts" | cut -d'|' -f3)
local init_output init_rc
if [ "$do_encrypt" = "TRUE" ]; then
if [ -z "$init_pass" ]; then
yad --error --title="$TITLE" --text="Empty passphrase."
return
fi
export BARESNAP_PASSPHRASE="$init_pass"
if [ "$cipher_algo" = "aes256gcm" ] || [ "$cipher_algo" = "aes" ]; then
init_output=$("$BARESNAP" init "$repo" --encrypt aes 2>&1)
else
init_output=$("$BARESNAP" init "$repo" --encrypt 2>&1)
fi
init_rc=$?
unset BARESNAP_PASSPHRASE
else
init_output=$("$BARESNAP" init "$repo" 2>&1)
init_rc=$?
fi
if [ $init_rc -ne 0 ] || [ ! -f "$repo/config" ]; then
yad --error --title="$TITLE" --text="Error initializing the repository:
$init_output"
return
fi
fi
# 2. Select source
local source
source=$(yad --file --directory --title="$TITLE - Folder to backup" --width=600 --height=400)
[ -z "$source" ] && return
# 3. Check if the repo is encrypted
local is_encrypted="FALSE"
local flags
flags=$(od -An -tx1 -j50 -N1 "$repo/config" 2>/dev/null | tr -d ' ')
if [ -n "$flags" ] && [ "$((16#$flags & 1))" -eq 1 ]; then
is_encrypted="TRUE"
fi
# 4. Ask for label and passphrase (if applicable)
local result
if [ "$is_encrypted" = "TRUE" ]; then
result=$(yad --form --title="$TITLE - Create backup" \
--text="Repo: $repo
Source: $source
(Encrypted Repository)" \
--field="Label" "" \
--field="Passphrase:H" "" \
--width=400)
local label
label=$(echo "$result" | cut -d'|' -f1)
local pass
pass=$(echo "$result" | cut -d'|' -f2)
[ -z "$pass" ] && return
export BARESNAP_PASSPHRASE="$pass"
run_with_progress "Create Backup" create "$repo" "$source" "$label"
unset BARESNAP_PASSPHRASE
else
result=$(yad --form --title="$TITLE - Create backup" \
--text="Repo: $repo
Source: $source" \
--field="Label" "" \
--width=400)
local label
label=$(echo "$result" | cut -d'|' -f1)
run_with_progress "Create Backup" create "$repo" "$source" "$label"
fi
}
do_restore() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
if [ ! -f "$repo/config" ]; then
yad --error --title="$TITLE" --text="The selected directory is not a valid repository."
return
fi
local snap
snap=$(ask_snap "$repo")
[ -z "$snap" ] && return
local target
target=$(ask_dir "Restore destination")
[ -z "$target" ] && return
local flags
flags=$(od -An -tx1 -j50 -N1 "$repo/config" 2>/dev/null | tr -d ' ')
if [ -n "$flags" ] && [ "$((16#$flags & 1))" -eq 1 ]; then
local pass
pass=$(yad --entry --title="$TITLE - Passphrase" \
--text="The repository is encrypted.
Enter the passphrase:" \
--hide-text --width=350)
[ -z "$pass" ] && return
export BARESNAP_PASSPHRASE="$pass"
fi
run_with_progress "Restore" restore "$repo" "$snap" "$target"
unset BARESNAP_PASSPHRASE
}
do_verify() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
if [ ! -f "$repo/config" ]; then
yad --error --title="$TITLE" --text="The selected directory is not a valid repository."
return
fi
run_with_progress "Verify" verify "$repo"
}
do_list() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
run_baresnap "List Snapshots" list "$repo"
}
do_ls() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
local snap
snap=$(ask_snap "$repo")
[ -z "$snap" ] && return
local result
result=$(yad --form --title="$TITLE - List files" \
--text="Snapshot: $(basename "$snap")" \
--field="Path prefix (optional)" "" \
--field="Recursive:CHK" "FALSE" \
--field="Long format:CHK" "FALSE" --width=400)
local prefix
prefix=$(echo "$result" | cut -d'|' -f1)
local recursive
recursive=$(echo "$result" | cut -d'|' -f2)
local longfmt
longfmt=$(echo "$result" | cut -d'|' -f3)
local args=(ls "$repo" "$snap")
[ -n "$prefix" ] && args+=("$prefix")
[ "$recursive" = "TRUE" ] && args+=("-r")
[ "$longfmt" = "TRUE" ] && args+=("-l")
run_baresnap "List Files" "${args[@]}"
}
# --- Interactive Snapshot Explorer ---
do_browse_snap() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
local snap
snap=$(ask_snap "$repo")
[ -z "$snap" ] && return
local ls_output
ls_output=$("$BARESNAP" ls "$repo" "$(basename "$snap")" -r -l 2>/dev/null)
if [ -z "$ls_output" ]; then
yad --error --title="$TITLE" --text="Could not list the snapshot or it is empty."
return
fi
local formatted
formatted=$(echo "$ls_output" | awk '{
type=$1; size=$2; date=$3" "$4;
path=""; for(i=5;i<=NF;i++) path=path $i " ";
sub(/ $/, "", path);
print type "|" size "|" date "|" path
}')
echo "$formatted" | yad --list \
--title="$TITLE - Explore $(basename "$snap")" \
--text="Snapshot content:" \
--column="Type" --column="Size" --column="Date/Time" --column="Path" \
--width=900 --height=600 \
--no-click --no-headers \
--button="Close:0"
}
do_extract() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
local snap
snap=$(ask_snap "$repo")
[ -z "$snap" ] && return
local target
target=$(ask_dir "Extraction destination")
[ -z "$target" ] && return
local result
result=$(yad --form --title="$TITLE - Extract files" \
--text="Snapshot: $(basename "$snap")
Destination: $target" \
--field="Paths to extract (empty = all)" "" --width=450)
local paths
paths=$(echo "$result" | cut -d'|' -f1)
local args=(extract "$repo" "$snap" "$target")
if [ -n "$paths" ]; then
for p in $paths; do args+=("$p"); done
fi
run_with_progress "Extract" "${args[@]}"
}
do_prune() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
local result
result=$(yad --form --title="$TITLE - Prune" \
--text="Repo: $repo" \
--field="Last copies:NUM" "5!0..100" \
--field="Days:NUM" "0!0..365" \
--field="Weeks:NUM" "0!0..52" \
--field="Months:NUM" "0!0..24" \
--field="Years:NUM" "0!0..10" \
--field="Simulation (does not delete):CHK" "TRUE" --width=400)
local keep_last
keep_last=$(echo "$result" | cut -d'|' -f1)
local keep_daily
keep_daily=$(echo "$result" | cut -d'|' -f2)
local keep_weekly
keep_weekly=$(echo "$result" | cut -d'|' -f3)
local keep_monthly
keep_monthly=$(echo "$result" | cut -d'|' -f4)
local keep_yearly
keep_yearly=$(echo "$result" | cut -d'|' -f5)
local dry_run
dry_run=$(echo "$result" | cut -d'|' -f6)
local args=(prune "$repo" --keep-last "$keep_last")
[ "$keep_daily" -gt 0 ] 2>/dev/null && args+=(--keep-daily "$keep_daily")
[ "$keep_weekly" -gt 0 ] 2>/dev/null && args+=(--keep-weekly "$keep_weekly")
[ "$keep_monthly" -gt 0 ] 2>/dev/null && args+=(--keep-monthly "$keep_monthly")
[ "$keep_yearly" -gt 0 ] 2>/dev/null && args+=(--keep-yearly "$keep_yearly")
[ "$dry_run" = "TRUE" ] && args+=(--dry-run)
run_with_progress "Prune" "${args[@]}"
}
do_info() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
run_baresnap "Info" info "$repo"
}
do_diff() {
local repo
repo=$(ask_repo)
[ -z "$repo" ] && return
local snap_a
snap_a=$(ask_snap "$repo")
[ -z "$snap_a" ] && return
local snap_b
snap_b=$(ask_snap "$repo")
[ -z "$snap_b" ] && return
run_baresnap "Diff" diff "$repo" "$snap_a" "$snap_b"
}
# ============================================================
# Main menu
# ============================================================
while true; do
choice=$(yad --list --title="$TITLE - Backup Manager" \
--text="Select an operation (double-click to execute):" \
--column="Operation" --column="Description" \
"1. Initialize Repository" "Create new repo (chacha20/AES-256-GCM + lz4/zstd)" \
"2. Create Backup" "Create snapshot (auto-initializes if necessary)" \
"3. Restore" "Restore a full snapshot" \
"4. Verify" "Verify repository integrity" \
"5. List Snapshots" "View available snapshots" \
"6. Explore Snapshot (Text)" "List files in a text window" \
"7. Interactive Explorer" "Browse files inside a snapshot (GUI)" \
"8. Extract Files" "Selective file extraction" \
"9. Prune" "Delete old snapshots" \
"10. Info" "Repository statistics" \
"11. Diff" "Compare two snapshots" \
"12. Exit" "Close the application" \
--width=700 --height=500 \
--listen)
rc=$?
[ $rc -eq 1 ] || [ $rc -eq 252 ] && break
[ -z "$choice" ] && continue
case "$choice" in
"1. Initialize Repository"*) do_init ;;
"2. Create Backup"*)       do_create ;;
"3. Restore"*)          do_restore ;;
"4. Verify"*)          do_verify ;;
"5. List Snapshots"*)   do_list ;;
"6. Explore Snapshot"*)  do_ls ;;
"7. Explorer"*)         do_browse_snap ;;
"8. Extract Files"*)   do_extract ;;
"9. Prune"*)              do_prune ;;
"10. Info"*)              do_info ;;
"11. Diff"*)              do_diff ;;
"12. Exit"*)             break ;;
esac
done
