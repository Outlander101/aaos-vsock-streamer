# Host Broker (VSOCK FRAM Relay)

This guide explains how to build, run, and integrate the **Host Broker**, which relays **FRAM‑framed H.264 video** from **AAOS VM** to **AGL Cluster VM** (or a generic Target VM) over **VSOCK**.

It covers:
*   Project structure
*   How FRAM framing works
*   Build & run instructions
*   Tests
*   Cross‑compiling

The host broker sits between:
*   **Target VM (e.g. AGL)**: connects *inbound* to the host broker first.
*   **AAOS VM**: broker connects *outbound* to the AAOS VM.

Once both ends are connected, the broker:
*   Forwards **FRAM‑framed H.264** bytes **1:1**.
*   Parses FRAM headers **only for logging**.
*   Automatically resets if AAOS or Target VM disconnects.

---

## Project Structure

    vsock_screenmirror_broker/
      CMakeLists.txt
      include/
        broker.hpp
        cli.hpp
        fram.hpp
        io.hpp
        log.hpp
        test_harness.hpp
        vsock.hpp
      src/
        broker.cpp
        cli.cpp
        fram.cpp
        main.cpp
        vsock.cpp
      tests/
        test_cli.cpp
        test_fram.cpp
        test_io.cpp
        test_main.cpp
      build/

---

## FRAM Format Overview

FRAM is a simple framing scheme used to wrap H.264 access units for streaming.

### FRAM Structure
*   **Header**: 16 bytes
*   **Payload**: `len` bytes (H.264 AU)

### Canonical Header Layout

| Offset | Field   | Type  |
| ------ | ------- | ----- |
| 0      | `len`   | `u32` |
| 4      | `pts`   | `u64` |
| 12     | `flags` | `u32` |

The broker never modifies the header or payload - bytes are forwarded as-is.

### Flags
*   `0x01` — KEYFRAME (IDR)
*   `0x04` — End‑of‑Stream (EOS)
*   `0x08` — CONFIG / CSD

---

## Build Instructions

### Native Build (x86_64 or aarch64)

```sh
cd vsock_screenmirror_broker
mkdir build && cd build

cmake -DBUILD_TESTS=ON ..
cmake --build . -j
```

- Outputs:
*   `build/host_broker`
*   `build/host_broker_tests` (if tests enabled)

---

## Running the Broker

### Typical Run (AAOS CID = 3)

```sh
cd vsock_screenmirror_broker/build

./host_broker \
  --host-listen-port 5000 \
  --android-cid 3 \
  --android-port 22345 \
  --android-timeout-ms 1500 \
  --retry-sleep-ms 500 \
  --verbose-headers
```

### Expected Log Sequence
1.  **Waiting for AGL** on host VSOCK port.
2.  AGL connects -> *“Target connected - now connecting to Android…”*
3.  Android connects -> *“Android connected - forwarding FRAM to Target.”*
4.  If AGL disconnects -> Android is dropped, broker resets.
5.  If Android disconnects -> AGL is dropped, broker resets.

---

## CLI Options

| Option                      | Description                        | Default |
| --------------------------- | ---------------------------------- | ------- |
| `--host-listen-port <port>` | Host VSOCK port for Target VM      | 5000    |
| `--android-cid <cid>`       | AAOS guest CID                     | 3       |
| `--android-port <port>`     | AAOS VSOCK port                    | 22345   |
| `--android-timeout-ms <ms>` | Timeout when connecting to AAOS    | 1500    |
| `--retry-sleep-ms <ms>`     | Sleep between retries              | 500     |
| `--verbose-headers`         | Enable FRAM header log dumps       | off     |
| `-h`, `--help`              | Show usage                         | —       |

---

## Tests

The project includes a unit test harness.

### Run Tests

```sh
cd build
./host_broker_tests
# Or via ctest:
ctest --output-on-failure
```

---

## Cross‑Compile for ARM (aarch64)

### Install compiler

```sh
sudo apt-get update
sudo apt-get install -y g++-aarch64-linux-gnu
```

### Build using Toolchain

```sh
cd vsock_screenmirror_broker
mkdir build-arm && cd build-arm

cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-aarch64.cmake -DBUILD_TESTS=OFF ..
cmake --build . -j
```

- Output:
*   `build-arm/host_broker`

---

## Notes & Assumptions
*   Target VM connects **into** the host broker.
*   AAOS must be listening on `--android-port`.
*   Broker suppresses SIGPIPE using `MSG_NOSIGNAL`.
