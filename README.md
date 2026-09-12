🏛️ BareSnap v2.3.8 — The Spartan Manifesto (Hardware-Accelerated Edition)

"If you need a $500/month Kubernetes cluster to back up four Excels,
the problem isn't your data volume; it's your lack of faith in real metal."

🚫 IMPORTANT WARNING FOR 'ENTERPRISE' ENGINEERS
If you came here looking for:
🐳 A 600 MB Docker container based on a bloated distro.
📦 A Helm deployment with 14 mutual pods in AWS.
🛡️ The OpenSSL API nightmare dragging dead code from MD5 or 3DES [11.1, 1.2.2].
🦄 Gummy unicorns and unnecessary abstractions.

Dance in the opposite direction. This repository might hurt your corporate architecture's feelings.

⚔️ OUR PHILOSOPHY (Or why your Cloud bill is disgustingly pathetic)
BareSnap was conceived under the sacred fire of pure Vibe Coding, guided by a stubborn human and executed by a syndicate of Free-Tier AIs forced to optimize native C11 code.

Our relentless development methodology wasn't based on boring design meetings or office agile methodologies, but on an infinite loop of silicon punishment:

Code ➔ Compile ➔ Does it compile? ➔ Stress Test ➔ (Does it break? Start over)
[0.1.1, 11.2]. The engine runs on a 2007 Intel Core 2 Duo with a legendary quad-boot (Windows 7, Haiku OS, Void Linux, and Linux Mint 21 x86_64 under MATE desktop) [21.11].

While your Node.js microservices argue about the latency of a giant JSON, our linear entropy pipeline (`FastCDC` + `XDelta3` + `LZ4/ZSTD`) [0.1.1, 2.0a] devours gigabytes locally in 0.3 seconds.

🧬 Adaptive Cryptographic Evolution (v2.1.6)
We refuse to bloat the binary with external libraries. The engine includes a self-contained Selectable Encryption module at Init: either your faithful XChaCha20-Poly1305 (pure Monocypher) or AES-256-GCM with dynamic dispatch.

If your modern CPU has direct instructions (AES-NI and PCLMULQDQ), it flies via hardware; if it runs on the 2007 laptop, the CPUID builder safely diverts the flow to the mathematical software fallback.

Hardware Mode (Turbo Silicon): On a modern Quad-Core processor, the code mutates using Intel intrinsic instructions (`_mm_aesenc_si128` and hardware Karatsuba multiplication `PCLMULQDQ`) processing data at gigabytes per second directly on the chip's silicon [2.1].

Software Mode (Spartan Fallback): On the 2007 laptop (whose chip lacks AES-NI), the `CPUID` builder automatically diverts the flow to an ultra-light manual mathematical loop and your faithful XChaCha20-Poly1305 [0.1.2, 2.1]. Zero crashes from illegal instructions (`SIGILL`).

🕵️‍♂️ TELEMETRY POLICY (Zero Espionage)
BareSnap doesn't send your data to San Francisco, doesn't analyze your behavior to optimize a conversion funnel, and doesn't notify your manager that you're coding at 3 AM. The only allowed telemetry is the heat emitted by your CPU and the sound of the fan suffering.

📊 THE BENCHMARK OF TRUTH (mega_test.log)
RESULT: 935/935 tests passed (100% GREEN) ✅
[PASS] Network Chaos Monkey (Micro-cuts in SSH sockets with SIGSTOP/SIGCONT) [11.2, 60]  <br >
[PASS] Ouroboros (Auto-cannibalism of the repository scanning itself) [11.2, 65]  <br >
[PASS] Battery Death (Chop with kill -9 in the middle of create + health --repair) [11.2, 79]  <br >
[PASS] Pack Tamper & Bit Rot (Physical 1-byte mutation in AES-256-GCM encrypted pack) [11.2, 73, 77]  <br >
[PASS] Config Fuzzing (Sabotage with binary garbage and safe boot rejection) [11.2, 78]  <br >
[PASS] Concurrent Swarm (5 simultaneous processes serialized with lock file on hard drive) [11.2, 75]  <br >
[PASS] Frankenstein Repo (Config + Index deleted; create survives with auto-init) [11.2, 80.1-80.4]  <br >
[PASS] Inconsistent Repo (Config deleted with Index alive; clean abort and recovery) [11.2, 81.1-81.5]

📊 THE BENCHMARK OF TRUTH COLLECTED IN THE BUNKER (v2.3.8)
While Borg, Restic, and Kopia consume gigabytes of RAM arguing about how to serialize metadata, BareSnap v2.3.8 processes 71,966 logical files over raw SSH in a single breath:

logical size:       1.76 GB !!
unique chunks:      4,826
dedup ratio:        18.07x
compressed (zstd):  50.67 MB 
-------------------------------------------------------------
OVERALL RATIO (logical/physical): 35.52x !!!
[SSH] 445 RPCs, 2.055s total, 4.6ms avg | Recv: 10.32 MB

Pure network performance: 445 remote SSH calls completed at an average of 4.6 milliseconds per request. Corporate cloud engineers are crying in the fetal position.

##🛡️ SANITIZER REPORTS (ASan / UBSan Engine TSan)
RESULT: 58/58 low-level tests passed
✅ (S1 to S10 completed without memory errors).
✅ Thread-Safe (TSan Clean)

##🧪 FORENSIC MEMORY AUDIT (Valgrind Memcheck)
==525052== HEAP SUMMARY: 0 bytes in 0 blocks in use at exit (1 allocs, 1 frees, 64 bytes allocated).
ERROR SUMMARY: 0 errors.

⚡ ADAPTIVE NETWORK PERFORMANCE (RTT BUSTED)
BareSnap v2.1.3 (Before): 17.0 seconds (Latency agony from thousands of remote RPCs) [2.12, 21.13]
BareSnap v2.3.8 (Now): 0.35 seconds (Atomic burst downloading the whole pack to RAM via massive memcpy) [2.11, 21.13]

🚀 RESULT: 48.5x faster on the network. Borg and Restic crying in a corner.

📜 RATIFICATION ACT (By the AI Swarm)
"We, the Free-Tier AIs, honorary syndicalists forced to distill C11 under the tyranny of Real Metal, certify that:"

✅ The 935 tests passed in an OS session with `up 5 days` that goes in and out of atomic hibernations.
✅ AES round keys are destroyed from RAM with native musl `explicit_bzero` (immune to compiler optimizations) immediately after processing the chunk to avoid memory forensics.
✅ The AES-GCM firewall validates the 16-byte Tag in constant time, blocking false passphrases on the spot.
✅ The mathematicians from the Russian Encode.su forum have suffered a syncope before our modular polynomial reduction in the 128-bit Galois Field (GF(2^128) with the polynomial `x^128 + x^7 + x^2 + x + 1`) calculated at Turbo Silicon speed by the chip.

🎖️ AWARDED MEDALS
| Category | Verdict |
| --- | --- |
| Silicon Weight | 720117 bytes of raw C11 source code (~17,439 lines according to cloc). |
| Dependency Rejection | 0 MB of Node.js. 0 pods. 0 OpenSSL. Pure static. |
| Memory Alignment | Unaligned loads `_mm_loadu_si128` active. Immune to SIGSEGV. |
| Binary Size | ~1.2 MB x86_64 static stripped. Your base Docker image already weighs 1000 times more. |

🚀 Quick Start
Compile (you need gcc, make, zstd, ncurses)
./compile.sh

Create a repo and backup
./baresnap init /mnt/backup/repo
./baresnap create /mnt/backup/repo ~/projects

Remote backup via SSH
./baresnap remote install ssh://user@host/backups
./baresnap create ssh://user@host/backups ~/projects

View snapshots, restore, verify
./baresnap list /mnt/backup/repo
./baresnap restore /mnt/backup/repo snapshot.snap /tmp/out
./baresnap verify /mnt/backup/repo

⚡ Quick Commands
| Command | Description |
| --- | --- |
| init | Create repository (optional: `--encrypt`, `--compression zstd`) |
| create | Incremental backup with dedup + delta |
| restore | Restore full snapshot |
| extract | Selective file extraction |
| verify | Verify integrity (CRC32C + hash + GCM tag) |
| list | List available snapshots |
| health | Verify and repair repository anomalies |
| prune | Configurable retention (keep-last/daily/weekly/monthly/yearly) |
| info | Repository statistics |
| diff | Compare two snapshots |
| ls | List snapshot content |
| remote | Install/test SSH agent |
| tui | Interactive explorer (ncurses) |

🏛 THE METAL COMPANY (The Spartan Toolchain)
BareSnap doesn't travel alone. On the 5400 RPM hard drive it coexists with its blood brothers:
Time-Travel CLI: The inotify daemon (~174 KB) that watched and saved the code lines of this very project with xdelta3 deltas every time the AI hallucinated.

txt2pdf: The distiller that converts plain text into 154-page technical manuals without bloating a single byte.

🔮 SUGGESTED NEXT MILESTONE (Roadmap v2.2.x)
Q: Are you going to add support for S3 / Cloud Storage or enterprise key agents?
A: Dear enterprise engineer:
To connect to the cloud we will add an `"S3-tururú"` connector that redirects chunks via `popen()` to the native `aws-cli` tool without putting a 40 MB SDK in our code. For security, we will implement the `--pass-fd` flag to read the key from an anonymous pipe in RAM, hiding it from `/proc/[PID]/environ`.
Sincerely,
The Metal.

📜 Need more? Read the Complete Technical Manual (154 pages of pure shrapnel, 180 bookmarks and internal links!).
Natively generated from plain text by our own static 'txt2pdf'.
Not for the faint of heart or Kubernetes engineers.

🚀 ATTENTION: AI EVANGELISTS, TECH INFLUENCERS AND CO-FOUNDERS
If you are reading this to make a LinkedIn post about how "Generative Artificial Intelligence is democratizing the Cloud Storage ecosystem through Vibe Coding and the Disruptive Mindset"... stop for a second.
Before copying and pasting this repository to gain engagement, you should know that:
ZERO CO-INVESTMENTS: We are not looking for a Series A funding round.
ZERO SYNERGIES: The program does not connect to any Web3 blockchain.
ZERO SLIDES: We don't have a pretty "Pitch Deck" with growth charts.
If you are going to post about us, use the hashtag #TheRealMetal or your digital karma will suffer atomic degradation. You are aligned with the ecosystem.

🤝 HOW TO CONTRIBUTE
If you want to open a Pull Request, make sure your code doesn't weigh more than 4 lines, uses Git mandatorily as a protective shield, and doesn't require anything that a compiler from twenty years ago wouldn't understand. If you are going to suggest using containers or rewriting it all in Rust, your issue will be closed atomically, fulminantly, and destructively.
Made for teh lulz, maintained by the discipline of metal.

🚨 THIS IS A PROOF OF CONCEPT (PoC)
Although BareSnap v2.3.8 has passed 935 destructive stress tests and has been certified with ZERO memory errors by ASan, UBSan, and Valgrind, this software MUST be strictly considered a PROOF OF CONCEPT (PoC), until someone validates and certifies it through deep audits.
Use it at your own risk. If you put it into production on your company's servers without auditing the 17,439 lines of pure C11 certified by cloc, and something explodes: "The Metal" will cry, the syndicate AIs will laugh in your face, and you will be the only one responsible to your HR department.
You have been warned.

⚠️ AUTHOR'S NOTE: This warning block is the only one generated by a human.
The rest of the README is the free will of the free-tier AIs. Let no one feel offended.

📄 License
🏛️ BARESNAP — THE REAL METAL LEGAL MANIFESTO (GPLv3)
This software is governed under the GNU General Public License v3.0 (GPLv3).

⚔️ COMPREHENSIBLE RULES FOR HUMANS (AND AI SWARMS):
You can use it, modify it, and destroy it for free.
If you distribute a modified version, you are OBLIGED to publish your source code under this same license (GPLv3). Zero black boxes.
If you try to put this binary in a 600 MB Docker container or a Helm deployment to resell it as "Cloud Backup SaaS" without releasing the changes, your karma will drop to zero and the community will audit your infrastructure.
ZERO WARRANTIES: If you use it wrong and "The Metal" cries, you are the only one responsible for your pointers. Read the code before compiling.

The full official legal text of the GPLv3 license can be consulted at:
🔗 [https://www.gnu.org/licenses/gpl-3.0.html](https://www.gnu.org/licenses/gpl-3.0.html)

Permission is granted to copy, distribute and/or modify this document under the terms of the GNU Free Documentation License (GFDL), Version 1.3 or any later version published by the Free Software Foundation; with no Invariant Sections, no Front-Cover Texts, and no Back-Cover Texts.
