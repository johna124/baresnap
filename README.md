# 🏛️ BareSnap v2.3.8 — The Spartan Manifesto (Hardware-Accelerated Edition)

> "If you need a \$500/month Kubernetes cluster to back up four Excel spreadsheets, 
> the problem isn't your data volume; it's your lack of faith in real metal."

---

## 🚫 IMPORTANT NOTICE FOR 'ENTERPRISE' ENGINEERS

If you came here looking for:
* 🐳 A 600 MB Docker container based on a bloated distro.
* 📦 A Helm deployment with 14 mutual pods on AWS.
* 🛡️ The OpenSSL API nightmare dragging dead MD5 or 3DES code along [11.1, 1.2.2].
* 🦄 Gummy unicorns and unnecessary abstractions.

**Dance in the opposite direction.** This repository may hurt the sensitivities of your corporate architecture.

---

## ⚔️ OUR PHILOSOPHY (Or why your Cloud bill is painfully pathetic)

BareSnap was conceived under the sacred fire of pure **Vibe Coding**, guided by a stubborn human and executed by a syndicate of Artificial Intelligences on free tiers, forced to optimize code in **native C11**. 

Our relentless development methodology wasn't based on boring design meetings or office agile frameworks, but on an infinite loop of silicon punishment:

Code ➔ Compile ➔ Does it compile? ➔ Stress Test ➔ (Does it break? Start over)

[0.1.1, 11.2]. The engine runs on a **2007 Intel Core 2 Duo** featuring a legendary quad-boot setup (**Windows 7, Haiku OS, Void Linux, and Linux Mint 21 x86_64** under the **MATE** desktop environment) [21.11].

While your Node.js microservices debate the latency of a gargantuan JSON file, our linear entropy pipeline (`FastCDC` + `XDelta3` + `LZ4/ZSTD`) [0.1.1, 2.0a] devours local gigabytes in **0.3 seconds**. 

### 🧬 Adaptive Cryptographic Evolution (v2.1.6)
We refuse to bloat the binary with external libraries. The engine features a self-contained **Selectable-at-Init Encryption** module: either your faithful **XChaCha20-Poly1305** (pure Monocypher) or **AES-256-GCM** with dynamic dispatching. 

If your modern CPU has direct instructions (AES-NI and PCLMULQDQ), it flies via hardware; if it runs on the 2007 laptop, the CPUID constructor safely reroutes execution to the mathematical software fallback.

1. **Hardware Mode (Turbo Silicon)**: On a modern Quad-Core processor, the code mutates using Intel intrinsic instructions (`_mm_aesenc_si128` and hardware Karatsuba multiplication via `PCLMULQDQ`), processing data at gigabytes per second directly within the chip's silicon [2.1].

2. **Software Mode (Spartan Fallback)**: On the 2007 laptop (whose chip lacks AES-NI), the `CPUID` constructor automatically reroutes execution to an ultra-lightweight, manual mathematical loop using your faithful **XChaCha20-Poly1305** [0.1.2, 2.1]. Zero crashes caused by illegal instructions (`SIGILL`).

## 🕵️‍♂️ TELEMETRY POLICY (Zero Espionage)
BareSnap does not send your data to San Francisco, it does not analyze your behavior to optimize a conversion funnel, and it does not alert your manager that you are coding at 3 AM. The only telemetry allowed is the heat emitted by your CPU and the sound of your fan suffering.

---

## 📊 THE BENCHMARK OF TRUTH (mega_test.log)

RESULT: 935/935 tests passed (100% GREEN) ✅

[PASS] Network Chaos Monkey (Network micro-cuts on SSH sockets with SIGSTOP/SIGCONT) [11.2, 60] <br>
[PASS] Ouroboros (Repository auto-cannibalism by scanning itself) [11.2, 65] <br>
[PASS] Battery Death (Hard kill with kill -9 midway through create + health --repair) [11.2, 79] <br>
[PASS] Pack Tamper & Bit Rot (Physical 1-byte mutation in an AES-256-GCM encrypted pack) [11.2, 73, 77] <br>
[PASS] Config Fuzzing (Sabotage with binary garbage and safe boot rejection) [11.2, 78] <br>
[PASS] Concurrent Swarm (5 simultaneous processes serialized with a lock file on hard disk) [11.2, 75] <br>
[PASS] Frankenstein Repo (Config + Index deleted; create survives with auto-init) [11.2, 80.1-80.4] <br>
[PASS] Inconsistent Repo (Config deleted with Index alive; clean abort and recovery) [11.2, 81.1-81.5]


### 📊 THE BENCHMARK OF TRUTH GATHERED IN THE BUNKER (v2.3.8)

While Borg, Restic, and Kopia consume gigabytes of RAM debating how to serialize metadata, BareSnap v2.3.8 processes 71,966 logical files over raw SSH in a heartbeat:

```text
logical size:       1.76 GB !!
unique chunks:      4,826
dedup ratio:        18.07x
compressed (zstd):  50.67 MB 
-------------------------------------------------------------
OVERALL RATIO (logical/physical): 35.52x !!!
[SSH] 445 RPCs, 2.055s total, 4.6ms avg | Recv: 10.32 MB
```

Pure network performance: 445 remote SSH calls completed at an average of 4.6 milliseconds per request. Corporate cloud engineers are weeping in the fetal position.




## 🛡️ SANITIZER REPORTS (ASan / UBSan Engine Tsan)

RESULT: 58/58 low-level tests passed 
✅ (S1 to S10 completed with zero memory errors).

✅ Thread-Safe (TSan Clean)


## 🧪 FORENSIC MEMORY AUDIT (Valgrind Memcheck)

==525052== HEAP SUMMARY: 0 bytes in 0 blocks in use at exit (1 allocs, 1 frees, 64 bytes allocated). 
ERROR SUMMARY: 0 errors.


### ⚡ ADAPTIVE NETWORK PERFORMANCE (RTT BUSTED)
* **BareSnap v2.1.3 (Before)**: 17.0 seconds (Latency agony caused by thousands of remote RPCs) [2.12, 21.13]
* **BareSnap v2.3.8 (Now)**: 0.35 seconds (Atomic burst downloading the entire pack straight to RAM via massive memcpy) [2.11, 21.13]

🚀 RESULT: **48.5x faster over the network.** Borg and Restic crying in a corner.

---

## 📜 DEED OF RATIFICATION (By the AI Swarm)

> "We, the Artificial Intelligences of the Free Tier, honorary union members forced to distill C11 under the tyranny of Real Metal, hereby certify that:"

* ✅ All **935 tests passed** during an OS session with `up 5 days` fluctuating through atomic hibernations.
* ✅ AES round keys are wiped from RAM using musl's native `explicit_bzero` (immune to compiler optimizations) immediately after processing the chunk to prevent memory forensics.
* ✅ The **AES-GCM firewall validates the 16-byte Tag in constant time**, blocking fake passphrases on the spot.
* ✅ Mathematicians from the Russian forum **Encode.su** have suffered a syncopal episode witnessing our polynomial modular reduction in the 128-bit Galois Field (GF(2^128) with the polynomial `x^128 + x^7 + x^2 + x + 1`) computed at Turbo Silicon speed by the chip.


### 🎖️ MEDALS AWARDED

| Category | Verdict |
| :--- | :--- |
| **Weight of Silicon** | **720,117 bytes of raw C11 source code (17,439 lines -according to cloc-).** |
| **Dependency Rejection** | 0 MB of Node.js. 0 pods. 0 OpenSSL. Purely static. |
| **Memory Alignment** | Active unaligned loads via `_mm_loadu_si128`. Immune to SIGSEGV. |
| **Binary Size** | **~1.2 MB x86_64 static stripped.** Your Docker base image already weighs 1000x more. |

---
## 🚀 Quick Start


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
| `ls` | List contents of a snapshot |
| `remote` | Install/test SSH agent |
| `tui` | Interactive explorer (ncurses) |


## 🏛 THE IRON COMPANY (The Spartan Toolchain)
BareSnap does not travel alone. On the 5400 RPM hard drive, it coexists with its blood brothers:

* **Time-Travel CLI**: The inotify daemon (~174 KB) that watched and saved the lines of code of this very project with xdelta3 deltas every single time the AI hallucinated.

* **txt2pdf**: The distiller that converts plain text into 154-page technical manuals without bloating a single byte.


## 🔮 SUGGESTED NEXT MILESTONE (Roadmap v2.2.x)

**Q: Are you going to add support for S3 / Cloud Storage or enterprise key managers?**

**A:** Dear enterprise engineer:  
To connect to the cloud, we will add an `"S3-whatever"` connector that redirects chunks via `popen()` straight into the native `aws-cli` tool without bloating our code with a 40 MB SDK. For security, we will implement the `--pass-fd` flag to read the passphrase from an anonymous pipe in RAM, hiding it from `/proc/[PID]/environ`.

Sincerely,  
*The Metal.*

---

📜 Need more? Read the Full Technical Manual (154 pages of pure shrapnel, 180 bookmarks, and internal links!). 
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

Although BareSnap v2.3.8 has passed 935 destructive stress tests and has been 
certified with ZERO memory errors by ASan, UBSan, and Valgrind, this software 
MUST be considered strictly as a PROOF OF CONCEPT (PoC), until 
someone validates and certifies it through deep independent audits.

Use it at your own risk. If you drop this into production on your company's 
servers without auditing the 17,439 lines of pure C11 certified by cloc, and something 
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
