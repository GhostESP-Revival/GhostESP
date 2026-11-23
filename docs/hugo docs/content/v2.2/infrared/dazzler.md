---
title: "IR Dazzler"
description: "Continuous IR flood mode for camera interference"
weight: 25
---

## Overview

IR Dazzler emits continuous IR at 38 kHz to overwhelm night-vision cameras. Runs in hardware with minimal CPU use.

> **Note:** Continuous IR can warm the LED and draw more current. Stop when not in use.

## Use from the UI

1. Open the **Infrared** view.
2. Select **IR Dazzler**. A popup appears and the LED begins emitting.
3. To stop, press the on-screen **Stop** button or press any enter/confirm input (touch, joystick, encoder, keyboard) while the popup is active.

The dazzler also stops automatically when you leave the Infrared view or run the global **stop** command.

## Use from the CLI

```
ir dazzler        # start
ir dazzler stop   # stop
```

The `stop` command also halts any active dazzler session.

## Compatibility

- Requires a board built with IR transmit hardware; the feature is hidden when `CONFIG_HAS_INFRARED` is off.
- Uses the configured IR LED pin (`CONFIG_INFRARED_LED_PIN`) with a 38 kHz carrier and high duty cycle flood.
