# NetSplit

A C++17 command-line downloader that splits one file into chunks and downloads them
in parallel over **two networks at once** (Wi-Fi + a phone's USB hotspot), with a
**hard limit on mobile data**, safe resume, and retries on flaky networks.

## The problem

In a hostel the Wi-Fi is slow and drops out, and the phone has a fast but small daily
data pack. Using only Wi-Fi is slow; using only the phone burns the whole pack.
NetSplit uses both at the same time and stops using the phone at a limit you choose,
so Wi-Fi finishes the rest:

```
netsplit get <url> --interfaces en0:4,en5:2 --mobile-limit 500MB
```

## What it does

- Parallel HTTP Range downloads (4 MB chunks) with a shared work queue: faster
  connections automatically take more chunks.
- Binds every worker to a network interface (`en0` Wi-Fi, `en5` iPhone USB).
- **Mobile data limit**: phone workers reserve each chunk against a budget before
  downloading it; every received byte is counted, also from attempts that fail.
- **Smart mode** (`--smart 2m`): the phone only joins if Wi-Fi alone would take longer.
- Checks every answer (status 206, `Content-Range`, length, ETag) — a wrong answer
  never reaches the file.
- Retries with exponential backoff + jitter, detects stalled connections (under
  1 KB/s for 20 s), hands a dropped network's chunks to the other network.
- Writes to `name.part`, saves progress to `name.progress.json` (write-ahead:
  data is fsync'ed before it is marked done), renames atomically when complete.
- Ctrl+C pauses; running the same command again resumes. SHA-256 check at the end.

## How it works

```
                 probe (Range: bytes=0-0) -> size, ETag
                                |
                 Planner: 25 chunks of 4 MB (inclusive ranges)
                                |
                     WorkQueue (mutex + condition_variable)
                  /        |         |          \
           worker 0    worker 1   worker 2    worker 3       each worker = own curl handle
           [en0 Wi-Fi] [en5 Phone][en0 Wi-Fi] [en5 Phone]    bound to its interface (IP_BOUND_IF)
                             |                      |
                     DataBudget.try_reserve(4 MB)   |        phone only: check + reserve
                             |                      |        under one lock
            HTTP GET Range -> check_response (206, Content-Range, length, ETag)
                                |
            pwrite(chunk.start + offset) into name.part      (no shared file position)
                                |
            StateFile: fsync data, then write progress.json (temp file + rename)
                                |
            all chunks done -> SHA-256 -> fsync -> rename name.part -> name
```

A failed chunk goes back into the queue: a network error gives it straight to any
free worker (another network can take it); a wrong server answer waits with backoff
and gives up after 5 tries.

## Build and run (macOS)

Needs CMake 3.16+, a C++17 compiler and libcurl (comes with macOS).
nlohmann/json is downloaded by CMake.

```bash
cmake -S . -B build && cmake --build build
./build/unit_tests                    # or: cd build && ctest

./build/netsplit interfaces           # en0 Wi-Fi, en5 iPhone USB (phone hotspot), ...
./build/netsplit get <url> --workers 4 --sha256 <expected>
./build/netsplit get <url> --interfaces en0:4,en5:2 --mobile-limit 500MB
./build/netsplit get <url> --interfaces en0:2,en5:2 --smart 2m --mobile-limit 200MB
```

Options: `--workers N`, `--interfaces en0,en5` or `en0:3,en5:2` (workers per network),
`--mobile-limit 500MB`, `--mobile-margin 5%` (kept free for TCP/TLS overhead),
`--smart 2m`, `--sha256 <hash>`, `--no-reuse`.

iPhone tip: connect the iPhone by USB with Mac Wi-Fi **off**, wait for a
`172.20.10.x` address, then turn Wi-Fi on (otherwise macOS often leaves
"iPhone USB" as *Attached* without an address).

## Benchmarks

All numbers are real runs on a MacBook Air, Linode Singapore test files, final
SHA-256 checked every time. MB = MiB (1,048,576 bytes).

**Mobile data limit (flagship)** — 1000 MiB file, same free Wi-Fi, two runs back to back

| | Wi-Fi alone (`en0:4`) | Wi-Fi + phone (`en0:4,en5:2 --mobile-limit 500MB`) |
|---|---|---|
| Time | 345.7 s | **263.7 s** (1.31× faster, 24% less time) |
| Average speed | 2.89 MB/s | 3.79 MB/s |
| Mobile data (NetSplit's count) | 0 MB | 472 MB of 500 MB (stops at 475 MB = limit − 5% margin) |
| Mobile data (macOS `netstat`, en5) | 0 MB | 497.0 MB received + 2.5 MB sent = 499.4 MB |
| Split | Wi-Fi 250 chunks | phone 118 chunks (47%), Wi-Fi 132 chunks (53%) |
| Retries | 0 | 0 |
| SHA-256 | matched | matched |

The phone did its 472 MB, hit the limit and stopped; Wi-Fi finished the other 528 MB.
`netstat` counts every byte on the phone interface, including TCP/IP and TLS headers:
25 MB (5.3%) more than NetSplit's own count. That is what the 5% margin is for, and the
phone's total stayed under 500 MB.

An earlier flagship run on an unstable hostel Wi-Fi (it went down 5 times, all recovered)
took 668.9 s with the same 472 MB / 528 MB split and a matching SHA-256.

**One file over two networks** — 100 MB, 4 workers in total, 3 runs each

| Network | run 1 (s) | run 2 (s) | run 3 (s) | avg (s) | avg MB/s |
|---|---|---|---|---|---|
| Wi-Fi only (`en0:4`) | 117.80 | 112.83 | 102.16 | 110.93 | 0.90 |
| Phone only (`en5:4`) | 5.91 | 5.29 | 5.18 | 5.46 | 18.32 |
| Both (`en0:2,en5:2`) | 8.47 | 10.87 | 9.20 | 9.51 | 10.52 |

"Both" was slower than the phone alone: the phone was ~20× faster, and the download
waited for the last 4 MB chunks on the slow Wi-Fi (see Limitations). With two
networks of similar speed (a Linux test setup with two simulated links of 3 and
6 MB/s, not the MacBook) both together was fastest: 14.6 s / 7.4 s / 5.9 s.

**Parallel connections and connection reuse** — 100 MB, Wi-Fi only

| Workers | Reuse | Time (s) | MB/s | Connections | vs 1 worker |
|---|---|---|---|---|---|
| 1 | on | 53.35 | 1.87 | 1 | 1.0× |
| 4 | on | 19.37 | 5.16 | 4 | 2.75× |
| 8 | on | 24.12 | 4.15 | 8 | 2.2× |
| 4 | off | 26.60 | 3.76 | 25 | 1.4× |

More connections help until the line is full; a new TCP + TLS handshake per chunk
cost ~37%.

**Smart mode** — 100 MB, both networks

| Command | Decision | Mobile data | Time |
|---|---|---|---|
| `--smart 5m` | Wi-Fi alone ~144 s ≤ 300 s → phone not used | 0 MB | 104.0 s |
| `--smart 30s --mobile-limit 20MB` | ~90 s > 30 s → phone joins after 5 s | 16 MB | 89.5 s |

**Failures (all ended with a matching SHA-256)**

| Test | Result |
|---|---|
| Wi-Fi switched off for ~10 s (100 MB) | 6 retries, finished in 54.3 s |
| Hotspot gone for ~1 hour (1000 MiB) | kept retrying, continued by itself, 300 retries |
| Wi-Fi changed to another network mid-download | workers reconnected on `en0`, finished |
| Phone unplugged mid-download | its chunks went to Wi-Fi, finished |
| Ctrl+C at 36%, run again with 4 workers | resumed 9 of 25 chunks, downloaded only 16 |
| Fault server (wrong status / range / length / ETag) | every wrong answer rejected |

**Tests:** 7 unit tests (planner, response validation, size parsing, DataBudget incl.
8 threads racing for the limit). Checked by planting 5 bugs — all 5 make the suite fail.

## Limitations

- macOS only: interface binding uses `IP_BOUND_IF`, names come from `networksetup`,
  SHA-256 from CommonCrypto. IPv4 only.
- The server must support HTTP Range; ETag is used to detect a changed file.
- The mobile limit is per run: a resumed download starts a new budget.
- The phone stops up to one chunk (4 MB) below the limit; the phone's own counter
  shows a little more than NetSplit (TCP/IP and TLS overhead, measured 5.3%). The 5%
  margin was just enough here because of that last unused chunk; for a hard guarantee
  use `--mobile-margin 7%`.
- Slow tail: when one network is much slower, the end of the download waits for its
  last chunks. Fix idea: endgame mode (the fast network also fetches the slow
  network's last chunks; first copy wins).
- A connection that goes silent is only detected after 20 s (stall timer).

## Project layout

```
include/ src/   http_client, probe, planner, storage, worker, work_queue, network,
                data_budget, resume_state, validate, progress, stop, phone_gate, hasher
tests/          unit_tests.cpp, fault_server.py (misbehaving test server), bench.sh
```
