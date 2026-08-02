---
title: "Development"
description: "Build firmware, Native SD Apps, and GhostScript extensions for GhostESP"
weight: 610
aliases:
  - "/development/"
---

Build GhostESP from source or extend an installed device without changing its firmware.

## Choose an extension model

| | [GhostScript]({{< relref "../ghostscript" >}}) | [Native SD Apps]({{< relref "native-sd-apps" >}}) |
|---|---|---|
| Best for | Automation, event-driven workflows, and chaining existing GhostESP features | Full interactive apps, custom UI, direct hardware access, and performance-sensitive work |
| Language | Lua 5.4 source compiled to `.gsb` | C compiled to a target-specific ESP-IDF shared object |
| Toolchain | Python and `ghostbt`; ESP-IDF is not required | Python, GBT, and ESP-IDF |
| Portability | One script package across supported boards, subject to available hardware | Build one package for each ESP target architecture |
| Runtime model | Constrained Lua environment with permission-checked GhostESP APIs | Trusted native code running in the firmware process |
| Device support | Designed to work with and without PSRAM | The on-device Apps gallery currently exposes third-party native apps only on PSRAM-equipped boards |

Start with **GhostScript** when the available Lua APIs can perform the job. Choose a **Native SD App** when you need custom widgets, lower-level peripherals, or native performance.

> Native app permissions gate the official SDK functions, but native apps are not sandboxed. Only install native packages from developers you trust.

## Development paths

- **[GhostScript]({{< relref "../ghostscript" >}})** — write, compile, install, and debug Lua automation packages.
- **[Native SD Apps]({{< relref "native-sd-apps" >}})** — build target-specific C apps with the native SDK.
- **[GBT]({{< relref "gbt" >}})** — use the current build, packaging, GhostScript compiler, firmware, and serial commands.
- **[Environment Setup]({{< relref "environment-setup" >}})** — prepare ESP-IDF for firmware and native app development.
- **[Custom Board Configurations]({{< relref "custom-board-configs" >}})** — add or adjust supported hardware.
