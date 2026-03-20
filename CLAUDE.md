# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Flipper Zero external application (FAP) for controlling OpenShock shockers via 433 MHz OOK Sub-GHz. Written in C using the FURI framework and Flipper Zero SDK. Supports 5 shocker models ported from the [OpenShock ESP32 firmware](../firmware).

## Build

Requires [UFBT](https://github.com/flipperdevices/flipperzero-ufbt) (installed via `pip install ufbt`).

```bash
python -m ufbt        # Build the FAP
python -m ufbt launch # Build and launch on connected Flipper Zero
```

No test or lint tooling is configured. Builds use `-Werror`.

## Architecture

- **`application.fam`** — FAP manifest (ID: `openshock_app`, category: Sub-GHz, libs: `subghz`)
- **`openshock.c`** — Main app: ViewPort-based UI for selecting shocker model, ID, channel, command type, and intensity. OK button triggers encode + transmit.
- **`protocols.h` / `protocols.c`** — OOK protocol encoders for all 5 shocker models. Each encoder converts command parameters into an array of `OokPulse` (high/low microsecond timing pairs). Ported from the ESP32 firmware's RMT-based encoders.
- **`transmit.h` / `transmit.c`** — Sub-GHz transmission using `subghz_devices` API with the internal CC1101 radio. Configures OOK 650kHz preset at 433.92 MHz, feeds pulse timings via async TX callback.
- **`images/`** — PNG assets auto-compiled into `<openshock_app_icons.h>` by UFBT

## Supported Shocker Models

| Model | Commands | Notes |
|-------|----------|-------|
| CaiXianlin | Shock, Vibrate, Sound, Light | 4-bit channel, intensity 0-99 |
| Petrainer | Shock, Vibrate, Sound | No channel select, intensity 0-100 |
| Petrainer998DR | Shock, Vibrate, Sound, Light | CH1/CH2 via channel param |
| T330 (WellturnT330) | Shock, Vibrate, Sound | CH1/CH2 via channel param |
| D80 | Shock, Vibrate, Sound | CH1/CH2/Both, 4-bit intensity (auto-scaled) |

Protocol details are documented in the [OpenShock wiki](https://wiki.openshock.org/hardware/shockers/caixianlin/#rf-specification) and the firmware encoder sources at `../firmware/src/radio/rmt/`.

## Key Flipper Zero Conventions

- Entry point signature: `int32_t openshock_app(void* p)`
- Core APIs: `<furi.h>` (OS primitives), `<furi_hal.h>` (hardware), `<gui/gui.h>` (UI framework)
- Sub-GHz device API: `<lib/subghz/devices/devices.h>` — use `subghz_devices_*` functions, not direct `furi_hal_subghz_load_preset` (which doesn't exist in the SDK)
- Stack size is 4096 bytes — avoid deep recursion and large stack allocations

## CI

GitHub Actions workflow (`.github/workflows/build.yml`) builds FAPs for both dev and release SDK channels using `flipperdevices/flipperzero-ufbt-action@v0.1`.
