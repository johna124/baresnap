# BareSnap v2.4.0 — The Spartan Manifesto (NIST-Vector Validated Edition)

> "If you need a \$500/month Kubernetes cluster to back up four Excel spreadsheets, 
> the problem isn't your data volume; it's your lack of faith in real metal."

---

## 🚫 IMPORTANT NOTICE FOR 'ENTERPRISE' ENGINEERS

If you came here looking for bloated containers, complex Helm deployments, legacy OpenSSL code, or unnecessary abstractions, **dance in the opposite direction**. This repository may hurt the sensitivities of your corporate architecture.

---

## ⚔️ OUR PHILOSOPHY (Or why your Cloud bill is painfully pathetic)

BareSnap was optimized in native C11 through an infinite loop of silicon punishment on a **2007 Intel Core 2 Duo** (quad-boot setup running Windows 7, Haiku OS, Void Linux, and Linux Mint 21 x86_64 under MATE). 

Our local entropy pipeline (`FastCDC` + `XDelta3` for local state + `LZ4/ZSTD`) devours storage gigabytes in **0.3 seconds** [2.1].

### 🧬 Adaptive Cryptographic Evolution (v2.4.0 Final)
The engine features a self-contained **Selectable-at-Init Encryption** module supporting **XChaCha20-Poly1305** or **AES-256-GCM**, both checked against known-answer vectors by a self-test that runs before any real encryption happens.


1. **Current path (Validated Software)**: AES-256-GCM and GHASH run through the portable C11 fallback on every platform right now — x86_64, ARM64, and the 2007 Core 2 Duo alike. AES-NI/PCLMULQDQ dispatch scaffolding exists in the codebase but is held behind a build flag until it passes its own NIST vectors independently of the software path. Correctness and constant-time tag comparison are identical either way; only throughput differs.

2. **Planned path (Hardware Mode)**: once the AES-NI/PCLMULQDQ dispatch clears validation, the existing `CPUID` runtime constructor switches to it automatically — no rebuild, no config change, and the SIGILL-safe fallback stays in place for anything without the extensions.

## 🕵️‍♂️ TELEMETRY POLICY (Zero Espionage)
No data sent to San Francisco, no conversion funnels analyzed, and no alerts sent to your manager. The only telemetry allowed is the heat emitted by your CPU and your suffering fan.

---

## 📊 THE BENCHMARK OF TRUTH (mega_test.log)

RESULT: 935/935 tests passed (100% GREEN) ✅ (Full details available in the referenced test logs).

[PASS] Network Chaos Monkey (Network micro-cuts on SSH sockets with SIGSTOP/SIGCONT) <br>
[PASS] Ouroboros (Repository auto-cannibalism by scanning itself)<br>
[PASS] Battery Death (Hard kill with kill -9 midway through create + health --repair)<br>
[PASS] Pack Tamper & Bit Rot (Physical 1-byte mutation in an AES-256-GCM encrypted pack)<br>
[PASS] Config Fuzzing (Sabotage with binary garbage and safe boot rejection)<br>
[PASS] Concurrent Swarm (5 simultaneous processes serialized with a lock file on hard disk) <br>
[PASS] Frankenstein Repo (Config + Index deleted; create survives with auto-init) <br>
[PASS] Inconsistent Repo (Config deleted with Index alive; clean abort and recovery) <br>


### 📊 THE BENCHMARK OF TRUTH GATHERED IN THE BUNKER (v2.4.0 Final)

While alternative tools allocate massive metadata structures in memory, BareSnap v2.4.0 processes 71,966 logical files over raw SSH via atomic serialization:

```text
logical size:       1.76 GB
unique chunks:      4,826
dedup ratio:        18.07x
compressed (zstd):  50.67 MB 
-------------------------------------------------------------
OVERALL RATIO (logical/physical): 35.52x
[SSH] 445 RPCs, 2.055s total, 4.6ms avg | Recv: 10.32 MB
```

Pure network performance: 445 remote SSH calls completed at an average of 4.6 milliseconds per request, optimizing network serialization on legacy CPU baselines.

## 🛡️ SANITIZER REPORTS (ASan / UBSan / TSan Hardened)

```text
[PASS] AddressSanitizer (ASan): 0 memory errors, safe pointer offsets.
[PASS] ThreadSanitizer  (TSan): Clean execution, 0 data races on SPSC circular queues.
[PASS] UndefinedBehaviorSanitizer (UBSan): Safe bitwise operations and valid unaligned loads.
[PASS] Subsystem Validation: Complete coverage on TUI, create, search, extract, and recovery commands.
```

## 🧪 FORENSIC MEMORY AUDIT (Valgrind Memcheck)

```text

== [104/112] 104. Valgrind: basic stress (init/create/verify/restore) ==
  [INFO] dataset: 10 MB random + 500 files (valgrind is ~20x slower)
  [PASS] 104.1 INIT without leaks: 0 leaks, 0 errors
  [PASS] 104.2 CREATE without leaks: 0 leaks, 0 errors
  [PASS] 104.3 VERIFY without leaks: 0 leaks, 0 errors
  [PASS] 104.4 RESTORE without leaks: 0 leaks, 0 errors
  [PASS] 104.5 restore under valgrind byte-identical

== [105/112] 105. Valgrind: extreme memory torture ==
  [INFO] dataset: 10 MB base + 500 unique files of 10 KB (impossible dedup)
  [PASS] 105.1 INIT torture without leaks: 0 leaks, 0 errors
  [PASS] 105.2 CREATE torture without leaks: 0 leaks, 0 errors
  [PASS] 105.3 VERIFY torture without leaks: 0 leaks, 0 errors
  [PASS] 105.4 RESTORE torture without leaks: 0 leaks, 0 errors
  [PASS] 105.5 unique file restored byte-identical

== [106/112] 106. Valgrind: aggressive prune (AES-256 + ZSTD) ==
  [PASS] 106.1 init --encrypt aes --compression zstd (level 3) [VERIFIED: BLAKE2B REAL ON THE REPO]
  [PASS] 106.2 5 encrypted snapshots created
  [PASS] 106.3 PRUNE AES+ZSTD without leaks: 0 leaks, 0 errors
  [PASS] 106.4 1 snapshot after prune
  [PASS] 106.5 verify after encrypted prune
  [PASS] 106.6 post-prune-valgrind: clean audit (5 packs, 1 idx, 1 blm, 0 tmp)
```
*Note: Memory profiles reflect zero-leak guarantees across strict block allocations and continuous POSIX subsystems.*

### ⚡ ADAPTIVE NETWORK PERFORMANCE (RTT LATENCY BUSTED)
* **BareSnap v2.1.3 (Before)**: 17.0 seconds (Latency bottlenecks caused by thousands of consecutive, small RPC calls)
* **BareSnap v2.4.0 (Now)**: 0.35 seconds (Atomic sequential burst reading the pack data straight into contiguous RAM buffers, slashing RTT overhead)

🚀 RESULT: **48.5x faster over the network.** High-latency WAN constraints bypassed successfully.
---


## 📜 DEED OF RATIFICATION (By the AI Swarm)

> "We, the Artificial Intelligences of the Free Tier, honorary union members forced to distill C11 under the tyranny of Real Metal, hereby certify that:"

* ✅ All **935 tests passed** during an OS session with `up 5 days` fluctuating through atomic hibernations.
* ✅ AES round keys are wiped from RAM using musl's native `explicit_bzero` (immune to compiler optimizations) immediately after processing the chunk to prevent memory forensics.
* ✅ The **AES-GCM firewall validates the 16-byte Tag in constant time**, blocking fake passphrases on the spot.
* ✅ Mathematicians from the Russian forum **Encode.su** have suffered a syncopal episode witnessing our polynomial modular reduction in the 128-bit Galois Field (GF(2^128) with the polynomial `x^128 + x^7 + x^2 + x + 1`) — running clean in portable C11 today, PCLMULQDQ dispatch waiting in the wings for its NIST clearance.


### 🎖️ MEDALS AWARDED
* **Weight of Silicon**: 766,469 bytes of raw C11 source code (18,351 lines according to `cloc`).
* **Dependency Rejection**: 0 dynamic links (`ldd` verified). Purely static compile-time enucleation.
* **Binary Size**: ~2.1 MB x86_64 static stripped.

---
## 🚀 Quick Start

```bash
# Compile (requires gcc, make, zstd, ncurses)
./compile.sh

# Create a repo and run a backup
./baresnap init /mnt/backup/repo
./baresnap create /mnt/backup/repo ~/projects

# Remote backup via SSH
./baresnap remote install ssh://user@host/backups
./baresnap create ssh://user@host/backups ~/projects

# View snapshots, restore, verify
./baresnap list /mnt/backup/repo
./baresnap restore /mnt/backup/repo snapshot.snap /tmp/out
./baresnap verify /mnt/backup/repo
```

## ⚡ Quick Commands

| Command | Description |
|---------|-------------|
| `init` | Create repository (optional: `--encrypt`, `--compression zstd`) |
| `create` | Incremental backup with dedup + delta |
| `restore` | Restore entire snapshot |
| `extract` | Selective file extraction |
| `verify` | Verify integrity (CRC32C + hash + GCM tag) |
| `list` | List available snapshots |
| `health` | Check and repair repository anomalies |
| `prune` | Configurable retention (keep-last/daily/weekly/monthly/yearly) |
| `info` | Repository statistics |
| `diff` | Compare two snapshots |
| `search` |    Search file paths across all snapshots |
| `ls` | List contents of a snapshot |
| `remote` | Install/test SSH agent |
| `tui` | Interactive explorer (ncurses) |


## ⚡ GLOBAL OPTIONS:

| Command | Description |
|---------|-------------|
| `--version, -v`        | Show version and exit |
| `--help, -h`          |  Show this help message |
| `--timeout <ms>`      |  SSH timeout per operation (default: 60000) |
| `--buffer-size <val>` |  Read buffer size per thread. |
|                       |Accepts K/k (KiB) or M/m (MiB) suffixes. |
|                       | Valid range: 4K .. 1G. Default: 256K.|
|                       | Only affects the create command. |


## 🏛 THE IRON COMPANY (The Spartan Toolchain)
BareSnap does not travel alone. On the 5400 RPM hard drive, it coexists with its blood brothers:

* **[Time-Travel 1.3 CLI](https://github.com/johna124/timetravel)**: The inotify daemon (~210 KB) that watched and saved the lines of code of this very project with xdelta3 deltas every single time the AI hallucinated.

* **[txt2pdf 1.6](https://github.com/johna124/baresnap/tree/main/docs/txt2pdf_v_1_6)**: The distiller that converts plain text into 169-page technical manuals without bloating a single byte.


## 🔮 SUGGESTED NEXT MILESTONE (Roadmap v2.4.x)

**Q: Are you going to add support for S3 / Cloud Storage or enterprise key managers?**

**A:** Dear enterprise engineer:  
To connect to the cloud, we will add an `"S3-whatever"` connector that redirects chunks via `popen()` straight into the native `aws-cli` tool without bloating our code with a 40 MB SDK. For security, we will implement the `--pass-fd` flag to read the passphrase from an anonymous pipe in RAM, hiding it from `/proc/[PID]/environ`.

Sincerely,  
*The Metal.*

---

📜 Need more? Read the Full Technical Manual (169 pages of pure shrapnel, 214 bookmarks, and internal links!). 
Generated natively from plain text by our very own static 'txt2pdf'.
Not for the faint of heart or Kubernetes engineers.


## 🚀 ATTENTION: AI EVANGELISTS, TECH INFLUENCERS, AND CO-FOUNDERS

If you are reading this to make a LinkedIn post about how "Generative Artificial 
Intelligence is democratizing the Cloud Storage ecosystem through Vibe Coding 
and a Disruptive Mindset"... stop for a second.

Before you copy and paste this repository to harvest engagement, you must know that:

* ZERO CO-INVESTMENTS: We are not looking for a Series A funding round.
* ZERO SYNERGIES: This program does not connect to any Web3 blockchain.
* ZERO SLIDES: We do not have a pretty "Pitch Deck" with growth charts.

If you are going to post about us, use the hashtag #TheRealMetal or your digital karma 
will suffer an atomic degradation. You have been aligned with the ecosystem.


## 🤝 HOW TO CONTRIBUTE
If you want to open a Pull Request, make sure your code does not exceed 4 lines, uses Git strictly as a protective shield, and requires nothing that a twenty-year-old compiler wouldn't understand. If you suggest using containers or rewriting everything in Rust, your *issue* will be closed atomically, instantly, and destructively.

*Made for the lulz, maintained by the discipline of metal.*


## 🚨 THIS IS A PROOF OF CONCEPT (PoC)

Although BareSnap v2.4.0 has passed 935 destructive stress tests and has been 
certified with ZERO memory errors by ASan, UBSan, Tsan, and Valgrind, this software 
MUST be considered strictly as a PROOF OF CONCEPT (PoC), until 
someone validates and certifies it through deep independent audits.

Use it at your own risk. If you drop this into production on your company's 
servers without auditing the  lines of pure C11 certified by cloc, and something 
explodes: "The Metal" will weep, the free-tier union AIs will laugh in your face, and 
you will be solely responsible to your HR department. You have been warned.

--------------------------------------------------------------------------------
⚠️ AUTHOR'S NOTE: This warning block is the only one generated by a human. 
The rest of the README is the free will of free-tier AIs. May no one be offended.



## 📄 License

# 🏛️ BARESNAP — THE REAL METAL LEGAL MANIFESTO (GPLv3)

This software is governed under the GNU General Public License v3.0 (GPLv3).

⚔️ **HUMAN-READABLE RULES (AND FOR AI SWARMS TOO):**
1. You can use it, modify it, and tear it apart for free.
2. If you distribute a modified version, you are **OBLIGATED** to publish your source code under this exact same license (GPLv3). Zero black boxes.
3. If you attempt to stuff this binary into a 600 MB Docker container or a Helm deployment to resell it as a "Cloud Backup SaaS" without releasing the changes, your karma will drop to zero and the community will audit your infrastructure.
4. **ZERO WARRANTIES:** If you misuse it and "The Metal" weeps, you are solely responsible for your own pointers. Read the code before compiling.

The complete and official legal text of the GPLv3 license can be found at:  
🔗 [https://www.gnu.org/licenses/gpl-3.0.html](https://www.gnu.org/licenses/gpl-3.0.html)

Permission is granted to copy, distribute and/or modify this document under the terms of the GNU Free Documentation License (GFDL), Version 1.3 or any later version published by the Free Software Foundation; with no Invariant Sections, no Front-Cover Texts, and no Back-Cover Texts.
