#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import mimetypes
import os
import secrets
import stat
import struct
import sys
import time
import urllib.error
import urllib.request
import webbrowser
import zlib
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


DEFAULT_BASE_URL = "https://ai-passport.folotoy.cn"
CONFIG_PATH = Path(os.getenv("FOLOTOY_PUBLISHER_CONFIG", Path.home() / ".config" / "folotoy" / "ai-passport-publisher.json"))
MAX_FIRMWARE_BYTES = 8 * 1024 * 1024
MAX_COVER_BYTES = 10 * 1024 * 1024
COVER_ASPECT_WIDTH = 3
COVER_ASPECT_HEIGHT = 4
SCREENSHOT_COMMAND = b"FAP_SCREENSHOT_V1\n"
SCREENSHOT_HEADER = "FAP_SCREENSHOT_V1"
MAX_SCREENSHOT_BYTES = 10 * 1024 * 1024
SCREENSHOT_RECEIPT_VERSION = 1
SCREENSHOT_RECEIPT_MAX_AGE_SECONDS = 24 * 60 * 60


class PublisherError(RuntimeError):
    pass


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


def rgb565_to_png(width: int, height: int, raw: bytes) -> bytes:
    expected = width * height * 2
    if len(raw) != expected:
        raise PublisherError(f"RGB565 screenshot length must be {expected} bytes; received {len(raw)}")
    scanlines = bytearray()
    offset = 0
    for _ in range(height):
        scanlines.append(0)
        for _ in range(width):
            pixel = raw[offset] | (raw[offset + 1] << 8)
            offset += 2
            red = ((pixel >> 11) & 0x1F) * 255 // 31
            green = ((pixel >> 5) & 0x3F) * 255 // 63
            blue = (pixel & 0x1F) * 255 // 31
            scanlines.extend((red, green, blue))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(bytes(scanlines), 9)) + png_chunk(b"IEND", b"")


def parse_screenshot_header(line: bytes) -> tuple[int, int, str, int]:
    try:
        marker, width_text, height_text, encoding, length_text = line.decode("ascii").strip().split()
        width, height, length = int(width_text), int(height_text), int(length_text)
    except (UnicodeDecodeError, ValueError) as exc:
        raise PublisherError("Device returned an invalid screenshot header") from exc
    encoding = encoding.upper()
    if marker != SCREENSHOT_HEADER or encoding not in {"PNG", "RGB565LE"}:
        raise PublisherError("Device returned an unsupported screenshot response")
    if not 1 <= width <= 4096 or not 1 <= height <= 4096:
        raise PublisherError("Device screenshot dimensions are invalid")
    expected = width * height * 2 if encoding == "RGB565LE" else None
    if length <= 0 or length > MAX_SCREENSHOT_BYTES or (expected is not None and length != expected):
        raise PublisherError("Device screenshot payload length is invalid")
    return width, height, encoding, length


def read_exact(stream: Any, length: int) -> bytes:
    payload = bytearray()
    while len(payload) < length:
        chunk = stream.read(length - len(payload))
        if not chunk:
            raise PublisherError("Device screenshot data ended before the declared length")
        payload.extend(chunk)
    return bytes(payload)


def screenshot_receipt_path(screenshot: Path) -> Path:
    return Path(f"{screenshot}.fap-capture.json")


def write_screenshot_receipt(screenshot: Path, width: int, height: int, port: str, captured_at: int | None = None) -> Path:
    receipt = screenshot_receipt_path(screenshot)
    receipt.write_text(json.dumps({
        "version": SCREENSHOT_RECEIPT_VERSION,
        "protocol": SCREENSHOT_HEADER,
        "source": "serial-framebuffer",
        "screenshot": screenshot.name,
        "sha256": hashlib.sha256(screenshot.read_bytes()).hexdigest(),
        "width": width,
        "height": height,
        "port": port,
        "capturedAt": captured_at if captured_at is not None else int(time.time()),
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return receipt


def validate_serial_capture(path: Path) -> dict[str, Any]:
    raw = path.read_bytes()
    if path.suffix.lower() != ".png" or not raw or len(raw) > MAX_SCREENSHOT_BYTES:
        raise PublisherError("Serial screen capture must be a non-empty PNG no larger than 10 MiB")
    width, height = image_dimensions(raw, path.suffix.lower())
    receipt_path = screenshot_receipt_path(path)
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise PublisherError(f"Serial capture receipt is missing: {receipt_path}") from exc
    except json.JSONDecodeError as exc:
        raise PublisherError("Serial capture receipt is invalid") from exc
    captured_at = receipt.get("capturedAt")
    now = int(time.time())
    if (
        receipt.get("version") != SCREENSHOT_RECEIPT_VERSION
        or receipt.get("protocol") != SCREENSHOT_HEADER
        or receipt.get("source") != "serial-framebuffer"
        or receipt.get("screenshot") != path.name
        or receipt.get("sha256") != hashlib.sha256(raw).hexdigest()
        or receipt.get("width") != width
        or receipt.get("height") != height
        or not receipt.get("port")
        or not isinstance(captured_at, int)
        or captured_at < now - SCREENSHOT_RECEIPT_MAX_AGE_SECONDS
        or captured_at > now + 300
    ):
        raise PublisherError("Serial capture receipt does not match a recent successful device capture")
    return {
        "path": str(path.resolve()),
        "receipt": str(receipt_path.resolve()),
        "size": len(raw),
        "sha256": receipt["sha256"],
        "width": width,
        "height": height,
        "capturedAt": captured_at,
        "source": "serial-framebuffer",
    }


def base_url() -> str:
    return os.getenv("FOLOTOY_AI_PASSPORT_URL", DEFAULT_BASE_URL).rstrip("/")


def api_request(path: str, method: str = "GET", payload: dict[str, Any] | None = None, token: str | None = None, body: bytes | None = None, content_type: str | None = None) -> tuple[int, dict[str, Any]]:
    headers = {"Accept": "application/json", "User-Agent": "folotoy-ai-passport-publisher/1.1"}
    data = body
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    elif content_type:
        headers["Content-Type"] = content_type
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(f"{base_url()}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=90) as response:
            raw = response.read()
            return response.status, json.loads(raw.decode("utf-8")) if raw else {"ok": True}
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            detail = json.loads(raw.decode("utf-8")) if raw else {"detail": f"HTTP {exc.code}"}
        except json.JSONDecodeError:
            detail = {"detail": raw.decode("utf-8", errors="replace") or f"HTTP {exc.code}"}
        return exc.code, detail
    except urllib.error.URLError as exc:
        raise PublisherError(f"Cannot reach {base_url()}: {exc.reason}") from exc


def load_config(required: bool = True) -> dict[str, Any]:
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except FileNotFoundError:
        if required:
            raise PublisherError("Not authorized. Run: publisher.py authorize")
        return {}
    if required and (not data.get("access_token") or data.get("base_url") != base_url()):
        raise PublisherError("Authorization is missing for this server. Run: publisher.py authorize")
    return data


def save_config(payload: dict[str, Any]) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    CONFIG_PATH.chmod(stat.S_IRUSR | stat.S_IWUSR)


def validate_esp_image(path: Path) -> dict[str, Any]:
    raw = path.read_bytes()
    if not raw or len(raw) > MAX_FIRMWARE_BYTES:
        raise PublisherError("Firmware must be non-empty and no larger than 8 MiB")
    if len(raw) < 33 or raw[0] != 0xE9:
        raise PublisherError("Firmware is not a recognizable merged ESP image")
    segment_count, flash_mode, flash_config = raw[1], raw[2], raw[3]
    if not 1 <= segment_count <= 16 or flash_mode > 3 or flash_config >> 4 > 4 or (flash_config & 0x0F) not in {0, 1, 2, 0x0F}:
        raise PublisherError("ESP image header parameters are invalid")
    offset = 24
    for _ in range(segment_count):
        if offset + 8 > len(raw):
            raise PublisherError("ESP segment header is incomplete")
        size = int.from_bytes(raw[offset + 4:offset + 8], "little")
        if size <= 0 or size > MAX_FIRMWARE_BYTES or offset + 8 + size > len(raw):
            raise PublisherError("ESP segment data is incomplete")
        offset += 8 + size
    if offset >= len(raw):
        raise PublisherError("ESP image checksum data is missing")
    return {"path": str(path.resolve()), "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest(), "segments": segment_count}


def image_dimensions(raw: bytes, suffix: str) -> tuple[int, int]:
    if suffix == ".png" and raw.startswith(b"\x89PNG\r\n\x1a\n") and len(raw) >= 24:
        return struct.unpack(">II", raw[16:24])
    if suffix in {".jpg", ".jpeg"} and raw.startswith(b"\xff\xd8"):
        offset = 2
        start_of_frame = {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}
        while offset + 4 <= len(raw):
            if raw[offset] != 0xFF:
                offset += 1
                continue
            while offset < len(raw) and raw[offset] == 0xFF:
                offset += 1
            if offset >= len(raw):
                break
            marker = raw[offset]
            offset += 1
            if marker in {0xD8, 0xD9} or 0xD0 <= marker <= 0xD7:
                continue
            if offset + 2 > len(raw):
                break
            segment_length = int.from_bytes(raw[offset:offset + 2], "big")
            if segment_length < 2 or offset + segment_length > len(raw):
                break
            if marker in start_of_frame and segment_length >= 7:
                height = int.from_bytes(raw[offset + 3:offset + 5], "big")
                width = int.from_bytes(raw[offset + 5:offset + 7], "big")
                return width, height
            offset += segment_length
    if suffix == ".webp" and len(raw) >= 30 and raw[:4] == b"RIFF" and raw[8:12] == b"WEBP":
        offset = 12
        while offset + 8 <= len(raw):
            chunk_type = raw[offset:offset + 4]
            chunk_size = int.from_bytes(raw[offset + 4:offset + 8], "little")
            payload = raw[offset + 8:offset + 8 + chunk_size]
            if len(payload) != chunk_size:
                break
            if chunk_type == b"VP8X" and len(payload) >= 10:
                return int.from_bytes(payload[4:7], "little") + 1, int.from_bytes(payload[7:10], "little") + 1
            if chunk_type == b"VP8 " and len(payload) >= 10 and payload[3:6] == b"\x9d\x01\x2a":
                return int.from_bytes(payload[6:8], "little") & 0x3FFF, int.from_bytes(payload[8:10], "little") & 0x3FFF
            if chunk_type == b"VP8L" and len(payload) >= 5 and payload[0] == 0x2F:
                packed = int.from_bytes(payload[1:5], "little")
                return (packed & 0x3FFF) + 1, ((packed >> 14) & 0x3FFF) + 1
            offset += 8 + chunk_size + (chunk_size & 1)
    raise PublisherError("Cover dimensions could not be read")


def validate_cover(path: Path) -> dict[str, Any]:
    suffix = path.suffix.lower()
    if suffix not in {".jpg", ".jpeg", ".png", ".webp"}:
        raise PublisherError("Cover must be JPEG, PNG, or WebP")
    raw = path.read_bytes()
    size = len(raw)
    if size <= 0 or size > MAX_COVER_BYTES:
        raise PublisherError("Cover must be non-empty and no larger than 10 MiB")
    width, height = image_dimensions(raw, suffix)
    if width * COVER_ASPECT_HEIGHT != height * COVER_ASPECT_WIDTH:
        raise PublisherError(f"Cover must use an exact portrait 3:4 aspect ratio; received {width}x{height}")
    return {"path": str(path.resolve()), "size": size, "sha256": hashlib.sha256(raw).hexdigest(), "width": width, "height": height, "aspectRatio": "3:4"}


def validate_source_repository_url(value: str) -> str:
    repository_url = value.strip()
    try:
        parsed = urlsplit(repository_url)
        hostname = parsed.hostname or ""
        _ = parsed.port
    except ValueError as exc:
        raise PublisherError("Source repository URL is invalid") from exc
    if parsed.scheme != "https" or not hostname or parsed.username or parsed.password:
        raise PublisherError("Source repository must use a public HTTPS URL")
    if parsed.query or parsed.fragment:
        raise PublisherError("Use the repository project page without query parameters or fragments")
    normalized_host = hostname.rstrip(".").lower()
    if normalized_host == "localhost" or normalized_host.endswith((".localhost", ".local")):
        raise PublisherError("Source repository must be publicly reachable")
    try:
        address = ipaddress.ip_address(normalized_host)
    except ValueError:
        address = None
    if address and (address.is_private or address.is_loopback or address.is_link_local or address.is_reserved or address.is_multicast or address.is_unspecified):
        raise PublisherError("Source repository must be publicly reachable")
    path_parts = [part for part in parsed.path.split("/") if part]
    if len(path_parts) < 2 or any(part in {".", ".."} for part in path_parts):
        raise PublisherError("Source repository must identify a complete project path")
    return repository_url.rstrip("/")


def multipart(fields: dict[str, str], files: dict[str, Path]) -> tuple[bytes, str]:
    boundary = f"----FoloToyPublisher{secrets.token_hex(12)}"
    chunks: list[bytes] = []
    for name, value in fields.items():
        chunks.extend([
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode(),
            value.encode("utf-8"), b"\r\n",
        ])
    for name, path in files.items():
        mime = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        safe_name = path.name.replace('"', "-")
        chunks.extend([
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="{name}"; filename="{safe_name}"\r\n'.encode(),
            f"Content-Type: {mime}\r\n\r\n".encode(),
            path.read_bytes(), b"\r\n",
        ])
    chunks.append(f"--{boundary}--\r\n".encode())
    return b"".join(chunks), f"multipart/form-data; boundary={boundary}"


def command_authorize(_: argparse.Namespace) -> None:
    status, request = api_request("/api/agent/device-code", method="POST", payload={})
    if status != 201:
        raise PublisherError(request.get("detail", f"Authorization failed: HTTP {status}"))
    print(f"Open this URL to authorize:\n{request['verificationUriComplete']}\n")
    print(f"Code: {request['userCode']}")
    webbrowser.open(request["verificationUriComplete"])
    deadline = time.time() + int(request.get("expiresIn", 600))
    interval = max(2, int(request.get("interval", 3)))
    while time.time() < deadline:
        status, result = api_request("/api/agent/token", method="POST", payload={"device_code": request["deviceCode"]})
        if status == 200:
            save_config({
                "base_url": base_url(),
                "access_token": result["accessToken"],
                "expires_at": int(time.time()) + int(result["expiresIn"]),
                "user": result.get("user", {}),
            })
            print(json.dumps({"ok": True, "authorized": result.get("user", {}), "config": str(CONFIG_PATH)}, ensure_ascii=False, indent=2))
            return
        if status != 428 or result.get("detail") != "authorization_pending":
            raise PublisherError(result.get("detail", f"Authorization failed: HTTP {status}"))
        time.sleep(interval)
    raise PublisherError("Authorization expired before it was approved")


def bearer() -> str:
    config = load_config()
    if int(config.get("expires_at", 0)) <= int(time.time()):
        raise PublisherError("Authorization expired. Run: publisher.py authorize")
    return str(config["access_token"])


def command_whoami(_: argparse.Namespace) -> None:
    status, result = api_request("/api/agent/me", token=bearer())
    if status != 200:
        raise PublisherError(result.get("detail", f"HTTP {status}"))
    print(json.dumps(result, ensure_ascii=False, indent=2))


def command_projects(_: argparse.Namespace) -> None:
    status, result = api_request("/api/agent/projects", token=bearer())
    if status != 200:
        raise PublisherError(result.get("detail", f"HTTP {status}"))
    print(json.dumps(result, ensure_ascii=False, indent=2))


def serial_module() -> Any:
    try:
        import serial
        import serial.tools.list_ports
    except ImportError as exc:
        raise PublisherError("Serial screen capture requires pyserial. Install it with: python3 -m pip install pyserial") from exc
    return serial


def command_capture_screen(args: argparse.Namespace) -> None:
    serial = serial_module()
    if args.list_ports:
        ports = [
            {"device": port.device, "description": port.description, "manufacturer": port.manufacturer or ""}
            for port in serial.tools.list_ports.comports()
        ]
        print(json.dumps({"ok": True, "ports": ports}, ensure_ascii=False, indent=2))
        return
    if not args.port:
        raise PublisherError("Choose a serial port with --port after running capture-screen --list-ports")
    if args.baud <= 0 or args.timeout <= 0:
        raise PublisherError("Serial baud and timeout must be positive")
    output = Path(args.output)
    if output.suffix.lower() != ".png":
        raise PublisherError("Captured screens must be saved to a .png path")
    try:
        with serial.Serial(args.port, baudrate=args.baud, timeout=min(args.timeout, 2), write_timeout=5) as stream:
            stream.reset_input_buffer()
            stream.write(SCREENSHOT_COMMAND)
            stream.flush()
            deadline = time.time() + args.timeout
            header = None
            while time.time() < deadline:
                line = stream.readline()
                if line.startswith(SCREENSHOT_HEADER.encode("ascii") + b" "):
                    header = line
                    break
            if header is None:
                raise PublisherError(
                    "The device did not answer the FAP_SCREENSHOT_V1 request. Use project screenshots or add the bundled serial screenshot protocol to the firmware."
                )
            width, height, encoding, length = parse_screenshot_header(header)
            payload = read_exact(stream, length)
    except PublisherError:
        raise
    except (OSError, ValueError) as exc:
        raise PublisherError(f"Unable to capture the device screen on {args.port}: {exc}") from exc
    if encoding == "RGB565LE":
        png = rgb565_to_png(width, height, payload)
    else:
        png_width, png_height = image_dimensions(payload, ".png")
        if (png_width, png_height) != (width, height):
            raise PublisherError("PNG screenshot dimensions do not match the device header")
        png = payload
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(png)
    receipt = write_screenshot_receipt(output, width, height, args.port)
    print(json.dumps({
        "ok": True,
        "path": str(output.resolve()),
        "receipt": str(receipt.resolve()),
        "width": width,
        "height": height,
        "source": "serial-framebuffer",
        "note": "Use this screen capture as gameplay evidence for a 3:4 cover; the physical device does not need to appear.",
    }, ensure_ascii=False, indent=2))


def command_validate(args: argparse.Namespace) -> None:
    firmware = validate_esp_image(Path(args.firmware))
    cover = validate_cover(Path(args.cover))
    screen_capture = validate_serial_capture(Path(args.screen_capture))
    print(json.dumps({"ok": True, "firmware": firmware, "cover": cover, "screenCapture": screen_capture}, ensure_ascii=False, indent=2))


def command_submit(args: argparse.Namespace) -> None:
    firmware_path = Path(args.firmware)
    cover_path = Path(args.cover)
    firmware = validate_esp_image(firmware_path)
    cover = validate_cover(cover_path)
    screen_capture = validate_serial_capture(Path(args.screen_capture))
    source_url = validate_source_repository_url(args.source_url) if args.source_url else ""
    fields = {
        "title_zh": args.title_zh,
        "description_zh": args.description_zh,
        "title_en": args.title_en or "",
        "description_en": args.description_en or "",
        "github_url": source_url,
    }
    preview_fields = {**fields}
    preview_fields["source_url"] = preview_fields.pop("github_url")
    preview = {"operation": "resubmit" if args.project_id else "create", "projectId": args.project_id, "fields": preview_fields, "firmware": firmware, "cover": cover, "screenCapture": screen_capture}
    if not args.confirmed:
        print(json.dumps({"ok": True, "preview": preview, "upload": False, "next": "Show this preview to the creator. After explicit approval, rerun with --confirmed."}, ensure_ascii=False, indent=2))
        return
    body, content_type = multipart(fields, {"cover": cover_path, "firmware": firmware_path})
    path = f"/api/agent/submissions/{args.project_id}/resubmit" if args.project_id else "/api/agent/submissions"
    status, result = api_request(path, method="POST", token=bearer(), body=body, content_type=content_type)
    if status not in {200, 201}:
        raise PublisherError(result.get("detail", f"Upload failed: HTTP {status}"))
    print(json.dumps(result, ensure_ascii=False, indent=2))


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="FoloToy AI Passport community firmware publisher")
    commands = root.add_subparsers(dest="command", required=True)
    commands.add_parser("authorize", help="Authorize through the official website").set_defaults(func=command_authorize)
    commands.add_parser("whoami", help="Show the authorized creator").set_defaults(func=command_whoami)
    commands.add_parser("projects", help="List the creator's submissions").set_defaults(func=command_projects)
    capture = commands.add_parser("capture-screen", help="Capture a supported device framebuffer over serial")
    capture.add_argument("--list-ports", action="store_true", help="List available serial ports")
    capture.add_argument("--port", help="Serial port selected by the creator")
    capture.add_argument("--baud", type=int, default=115200)
    capture.add_argument("--timeout", type=float, default=15.0)
    capture.add_argument("--output", default="ai-passport-screen.png")
    capture.set_defaults(func=command_capture_screen)
    validate = commands.add_parser("validate", help="Validate a cover and merged firmware without uploading")
    validate.add_argument("--firmware", required=True)
    validate.add_argument("--cover", required=True)
    validate.add_argument("--screen-capture", required=True, help="Recent PNG created by capture-screen, with its adjacent receipt")
    validate.set_defaults(func=command_validate)
    submit = commands.add_parser("submit", help="Preview or submit a new project or revision")
    submit.add_argument("--title-zh", required=True)
    submit.add_argument("--description-zh", required=True)
    submit.add_argument("--title-en", default="")
    submit.add_argument("--description-en", default="")
    submit.add_argument("--source-url", "--github-url", dest="source_url", default="", help="Optional public HTTPS Git repository project page")
    submit.add_argument("--cover", required=True)
    submit.add_argument("--firmware", required=True)
    submit.add_argument("--screen-capture", required=True, help="Recent PNG created by capture-screen, with its adjacent receipt")
    submit.add_argument("--project-id", type=int)
    submit.add_argument("--confirmed", action="store_true", help="Upload after the creator explicitly approved the preview")
    submit.set_defaults(func=command_submit)
    return root


def main() -> int:
    try:
        args = parser().parse_args()
        args.func(args)
        return 0
    except (PublisherError, FileNotFoundError, PermissionError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
