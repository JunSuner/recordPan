"""End-to-end test of the recordPan BLE protocol (docs/BLE.md).

Scans for recordPan-XXXX, reads status, lists files, then starts a recording over BLE,
streams live audio for --seconds (with one mark halfway), stops, and saves the received
Opus packets to --out as JSON for analysis.

Usage: python ble_test.py [--seconds 12] [--out ble_audio.json]
Requires: pip install bleak
"""
import argparse
import asyncio
import json
import struct
import time

from bleak import BleakClient, BleakScanner


def uuid(n):
    return f"6a2e{n:04x}-8b1f-4c3e-9d2a-7265636f7264"


SVC, CONTROL, STATUS, FILES, AUDIO = (uuid(i) for i in range(1, 6))
CMD_START, CMD_STOP, CMD_MARK, CMD_LIST = 1, 2, 3, 4
STATES = {0: "idle", 1: "recording", 2: "no storage"}


def parse_status(b):
    ver, state, marks, elapsed, free_kb, peak = struct.unpack_from("<BBHIIb", b)
    return {"ver": ver, "state": STATES.get(state, state), "marks": marks,
            "elapsed_ms": elapsed, "free_kb": free_kb, "peak_db": peak}


async def main(args):
    print("Scanning...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "").startswith("recordPan") or SVC in ad.service_uuids, timeout=15)
    if not dev:
        print("No recordPan device found")
        return
    print("Found", dev.name, dev.address)

    async with BleakClient(dev) as client:
        print("Connected, MTU", client.mtu_size)
        print("Status (read):", parse_status(await client.read_gatt_char(STATUS)))

        statuses, files, audio = [], [], []
        files_done = asyncio.Event()

        def on_status(_, data):
            statuses.append((time.time(), parse_status(bytes(data))))

        def on_files(_, data):
            data = bytes(data)
            size = struct.unpack_from("<I", data)[0]
            if size == 0xFFFFFFFF and len(data) == 4:
                files_done.set()
            else:
                files.append((data[4:].decode(), size))

        def on_audio(_, data):
            data = bytes(data)
            seq, n = struct.unpack_from("<HB", data)
            pos, pkts = 3, []
            for _ in range(n):
                ln = data[pos]
                pkts.append(data[pos + 1:pos + 1 + ln].hex())
                pos += 1 + ln
            audio.append({"t": time.time(), "seq": seq, "packets": pkts, "bytes": len(data)})

        await client.start_notify(STATUS, on_status)
        await client.start_notify(FILES, on_files)
        await client.start_notify(AUDIO, on_audio)

        await client.write_gatt_char(CONTROL, bytes([CMD_LIST]), response=True)
        await asyncio.wait_for(files_done.wait(), 10)
        print(f"Files ({len(files)}):")
        for name, size in files:
            print(f"  {name:20} {size:>10,}")

        print(f"START, recording {args.seconds} s - talk now")
        await client.write_gatt_char(CONTROL, bytes([CMD_START]), response=True)
        await asyncio.sleep(args.seconds / 2)
        await client.write_gatt_char(CONTROL, bytes([CMD_MARK]), response=True)
        await asyncio.sleep(args.seconds / 2)
        await client.write_gatt_char(CONTROL, bytes([CMD_STOP]), response=True)
        await asyncio.sleep(2)

        print(f"Status notifications: {len(statuses)}")
        for _, s in statuses:
            print("  ", s)
        frames = sum(len(a["packets"]) for a in audio)
        print(f"Audio notifications: {len(audio)}, packets: {frames}, "
              f"bytes: {sum(a['bytes'] for a in audio)}")
        with open(args.out, "w") as f:
            json.dump({"mtu": client.mtu_size, "files": files, "audio": audio}, f)
        print("Saved", args.out)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--seconds", type=float, default=12)
    p.add_argument("--out", default="ble_audio.json")
    asyncio.run(main(p.parse_args()))
