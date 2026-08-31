# Required serial screen capture protocol

`capture-screen` retrieves the running display framebuffer as evidence for a gameplay-first cover. It never takes a photograph of the enclosure and does not require the enclosure to appear in the cover.

The command uses `pyserial`. If it is not installed, install the bundled dependency with `python3 -m pip install -r requirements.txt`. Publisher validation and submission require a recent successful capture.

## Host request

The publisher opens the creator-selected serial port at 115200 baud by default and writes this ASCII line:

```text
FAP_SCREENSHOT_V1\n
```

The command must be observational only. It must not reboot, flash, erase, change settings, or expose device credentials.

## Device response

The firmware returns one ASCII header followed immediately by the declared binary payload:

```text
FAP_SCREENSHOT_V1 <width> <height> <encoding> <byte_length>\n
<binary payload>
```

Supported encodings:

- `RGB565LE`: little-endian RGB565 pixels in row-major order. `byte_length` must equal `width * height * 2`.
- `PNG`: a complete PNG whose dimensions match `width` and `height`.

Width and height must each be 1–4096 pixels and the payload must not exceed 10 MiB. On success the publisher saves a PNG without changing the connected device.

## Availability and enforcement

Firmware published through this Skill must implement the protocol. A successful capture writes the PNG plus an adjacent `.fap-capture.json` receipt containing the screenshot hash, dimensions, port, protocol, and capture time. `validate` and `submit` require the matching receipt and reject captures older than 24 hours. Existing firmware that only emits logs must add the protocol before it can be published with this Skill.

Before using a captured screen in a public cover, inspect it for device KEY values, passwords, personal information, tokens, or other secrets. Do not publish or feed sensitive captures into image generation.
