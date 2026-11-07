#!/usr/bin/env python3
"""Debug monitor for the K-pop wand: connects over BLE (Nordic UART), prints every line
the wand sends, and lets you type protocol commands (C:A020F0, F:FFFFFF:3, P:120, OFF, D:1 ...).

  pip install bleak
  python3 tools/wand_monitor.py            # gestures only
  python3 tools/wand_monitor.py --telemetry  # also stream T: orientation lines (10 Hz)
"""
import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # laptop -> wand (write)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # wand -> laptop (notify)


async def main(name: str, telemetry: bool) -> None:
    print(f"Scanning for {name!r}...")
    dev = await BleakScanner.find_device_by_name(name, timeout=15)
    if dev is None:
        sys.exit(f"{name} not found. Is the wand powered and not connected to another app?")
    buf = bytearray()

    def on_rx(_, data: bytearray) -> None:
        buf.extend(data)
        while b"\n" in buf:
            line, _, rest = bytes(buf).partition(b"\n")
            buf[:] = rest
            text = line.decode(errors="replace").strip()
            if text.startswith("T:") and text != "T:NOIMU":
                r, p, gl, gz, mag = text[2:].split(",")
                print(f"roll {int(r):5d}  pitch {int(p):5d}  gyroLong {int(gl):6d}  "
                      f"gyroZ {int(gz):6d}  |a| {int(mag) / 100:.2f} g")
            else:
                print(f"<< {text}")

    async with BleakClient(dev) as client:
        print(f"Connected to {dev.address}. Type commands, Ctrl+C to quit.")
        await client.start_notify(NUS_TX, on_rx)

        async def send(cmd: str) -> None:
            await client.write_gatt_char(NUS_RX, (cmd + "\n").encode(), response=False)
            print(f">> {cmd}")

        if telemetry:
            await send("D:1")
        loop = asyncio.get_running_loop()
        while client.is_connected:
            line = await loop.run_in_executor(None, sys.stdin.readline)
            if not line:
                await asyncio.Event().wait()  # stdin closed: keep listening
            if line.strip():
                await send(line.strip())


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="KpopWand")
    ap.add_argument("--telemetry", action="store_true")
    args = ap.parse_args()
    try:
        asyncio.run(main(args.name, args.telemetry))
    except KeyboardInterrupt:
        pass
