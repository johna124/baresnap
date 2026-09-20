When you see the split interface:
Move with the up and down arrows.
Switch panels with the TAB key.
Press F1 to open help and ESC to close it.
Exit the application by pressing F10 or Ctrl+Q.

1. Create clean test environment and data
mkdir -p /tmp/vg_tui_test/fuente && cd /tmp/vg_tui_test
echo "test file 1" > ./fuente/archivo1.txt
echo "test file 2" > ./fuente/archivo2.txt

2. Set the passphrase and initialize the AES-encrypted repository
export BARESNAP_PASSPHRASE="ClaveEspartanaConAES256"
export TERM=xterm-256color
~/newrepo/baresnap_v2_3_4/baresnap init ./repo --encrypt aes --compression zstd --zstd-level 5

3. Make an initial backup to have snapshots to navigate in the TUI
~/newrepo/baresnap_v2_3_4/baresnap create ./repo ./fuente

4. Launch the TUI under Valgrind saving the report in an isolated log
valgrind --leak-check=full --show-leak-kinds=all --log-file=./valgrind_tui.log ~/newrepo/baresnap_v2_3_4/baresnap tui ./repo
