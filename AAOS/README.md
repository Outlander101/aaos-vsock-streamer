# AAOS Integration

This directory contains the necessary source code to enable the native `screentransfer` service within an **Android Automotive OS (AAOS)** image.

## Directory Layout

| Directory/File |   Description |
|------|-------------|
| `screentransfer/` | Contains the `screentransfer` native service code implementation (C++), `Android.bp`, and `sepolicy` rules. |

## Instructions

The `screentransfer` service is responsible for capturing the UI from SurfaceFlinger (Display 1), encoding it into H.264, wrapping it in FRAM protocol, and sending it out over a VSOCK connection.

To integrate this into your AAOS device build:
1. Copy the `screentransfer` directory to `frameworks/av/cmds/screentransfer` in your AOSP/AAOS source tree.
2. See the detailed build and usage documentation inside [screentransfer/README.md](./screentransfer/README.md).
3. Update your device's `BoardConfig.mk` and `device.mk` (or product makefiles) to include the `screentransfer` SEPolicy and product packages as documented in the root repository README.
