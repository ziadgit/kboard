"""BTS Wand bridge: BLE (KpopWand, Nordic UART) -> WebSocket -> browser display.
Run: python wand_bridge.py        (real wand)
     python wand_bridge.py --fake (type gestures like SHAKE, TWIST)
Then open http://localhost:8000
"""
import asyncio, sys, threading, functools, http.server, os, webbrowser
import websockets

NAME = "KpopWand"
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # wand -> laptop (notify)
RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # laptop -> wand (write)
clients = set()

async def broadcast(msg):
    print(">>", msg)
    for ws in list(clients):
        try: await ws.send(msg)
        except Exception: clients.discard(ws)

async def ws_handler(ws, *args):
    clients.add(ws)
    try: await ws.wait_closed()
    finally: clients.discard(ws)

def serve_http():
    d = os.path.dirname(os.path.abspath(__file__))
    h = functools.partial(http.server.SimpleHTTPRequestHandler, directory=d)
    h.log_message = lambda *a: None
    http.server.ThreadingHTTPServer(("localhost", 8000), h).serve_forever()

async def ble_loop():
    from bleak import BleakScanner, BleakClient
    while True:
        await broadcast("STATUS:scanning…")
        dev = await BleakScanner.find_device_by_name(NAME, timeout=10)
        if not dev:
            continue
        try:
            async with BleakClient(dev) as c:
                await broadcast("STATUS:connected 💜")
                buf = bytearray()
                loop = asyncio.get_running_loop()
                def on_data(_, data):
                    buf.extend(data)
                    while b"\n" in buf:
                        line, _, rest = bytes(buf).partition(b"\n")
                        buf[:] = rest
                        s = line.decode(errors="ignore").strip()
                        if s:
                            loop.create_task(handle(c, s))
                await c.start_notify(TX, on_data)
                await c.write_gatt_char(RX, b"D:1\n", response=False)  # stream orientation for color sphere
                while c.is_connected:
                    await asyncio.sleep(1)
        except Exception as e:
            print("BLE error:", e)
        await broadcast("STATUS:disconnected")

async def handle(client, line):
    if line.startswith("T:"):
        for ws in list(clients):
            try: await ws.send(line)
            except Exception: clients.discard(ws)
        return
    await broadcast(line)
    if line.startswith("G:"):
        try: await client.write_gatt_char(RX, b"F:A020F0:2\n", response=False)
        except Exception as e: print("flash failed:", e)

async def fake_loop():
    await broadcast("STATUS:fake mode")
    loop = asyncio.get_running_loop()
    print("Type a gesture (SHAKE, TILT_LEFT, TILT_RIGHT, POINT_UP, TWIST, WAVE):")
    while True:
        g = (await loop.run_in_executor(None, sys.stdin.readline)).strip().upper()
        if g: await broadcast(g if g.startswith("G:") else "G:" + g)

async def main():
    threading.Thread(target=serve_http, daemon=True).start()
    async with websockets.serve(ws_handler, "localhost", 8765):
        print("Display at http://localhost:8000")
        webbrowser.open("http://localhost:8000/" + ("concert.html" if "--concert" in sys.argv else ""))
        await (fake_loop() if "--fake" in sys.argv else ble_loop())

if __name__ == "__main__":
    asyncio.run(main())
