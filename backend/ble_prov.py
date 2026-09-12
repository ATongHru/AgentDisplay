"""BLE provision client for AgentDisplay (Nordic UART / NUS)."""

from __future__ import annotations

import asyncio
import socket
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # PC -> device (write)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device -> PC (notify)
DEVICE_NAME = "AgentDisplay"

def _norm_ip(value: str | None) -> str:
    text = (value or "").strip()
    if not text:
        return ""
    parts = text.split(".")
    if len(parts) != 4:
        raise ValueError(f"无效 IPv4: {text}")
    for part in parts:
        n = int(part)
        if n < 0 or n > 255:
            raise ValueError(f"无效 IPv4: {text}")
    return text


def _bleak_available() -> tuple[bool, str]:
    try:
        import bleak  # noqa: F401

        return True, ""
    except Exception as exc:  # pragma: no cover
        return False, str(exc)


def guess_lan_ip() -> str:
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.3)
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        sock.close()
        if ip and not ip.startswith("127."):
            return ip
    except OSError:
        pass
    try:
        return socket.gethostbyname(socket.gethostname())
    except OSError:
        return "127.0.0.1"


@dataclass
class BleProvService:
    """Serialize BLE ops; Windows radio cannot safely multiplex."""

    _lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    _busy: bool = False
    _last_scan: list[dict[str, Any]] = field(default_factory=list)
    _log: deque[dict[str, Any]] = field(default_factory=lambda: deque(maxlen=80))

    def _push(self, level: str, msg: str) -> None:
        item = {"ts": time.time(), "level": level, "msg": msg}
        self._log.appendleft(item)
        print(f"[ble_prov] {level}: {msg}")

    def status(self) -> dict[str, Any]:
        ok, err = _bleak_available()
        return {
            "bleak_ok": ok,
            "bleak_error": err,
            "busy": self._busy,
            "device_name": DEVICE_NAME,
            "last_scan": list(self._last_scan),
            "logs": list(self._log),
            "suggested_host": guess_lan_ip(),
            "suggested_port": 8000,
        }

    async def scan(self, timeout: float = 6.0) -> list[dict[str, Any]]:
        ok, err = _bleak_available()
        if not ok:
            raise RuntimeError(f"bleak 未安装或不可用: {err}")
        from bleak import BleakScanner

        async with self._lock:
            self._busy = True
            try:
                self._push("info", f"扫描 BLE（{timeout:.0f}s），请确保设备已长按 BOOT 进入配网")
                found = await BleakScanner.discover(timeout=timeout, return_adv=True)
                devices: list[dict[str, Any]] = []
                for addr, (dev, adv) in found.items():
                    name = dev.name or (adv.local_name if adv else None) or ""
                    rssi = adv.rssi if adv and adv.rssi is not None else getattr(dev, "rssi", None)
                    if name and DEVICE_NAME.lower() in name.lower():
                        devices.append({"address": addr, "name": name, "rssi": rssi})
                devices.sort(key=lambda d: d.get("rssi") or -999, reverse=True)
                self._last_scan = devices
                if devices:
                    self._push(
                        "info",
                        f"找到 {len(devices)} 台: " + ", ".join(d["address"] for d in devices),
                    )
                else:
                    self._push("warn", f"未找到 {DEVICE_NAME}，请长按 BOOT 3 秒后再扫")
                return devices
            finally:
                self._busy = False

    async def provision(
        self,
        *,
        ssid: str,
        password: str,
        host: str,
        port: int,
        address: str | None = None,
        ip: str | None = None,
        netmask: str | None = None,
        gateway: str | None = None,
        scan_timeout: float = 6.0,
        reply_timeout: float = 8.0,
    ) -> dict[str, Any]:
        ok, err = _bleak_available()
        if not ok:
            raise RuntimeError(f"bleak 未安装或不可用: {err}")
        from bleak import BleakClient, BleakScanner

        ssid = (ssid or "").strip()
        password = password or ""
        host = (host or "").strip()
        if not ssid:
            raise ValueError("WiFi SSID 不能为空")
        if not host:
            raise ValueError("后端 Host/IP 不能为空")
        if not (1 <= int(port) <= 65535):
            raise ValueError("端口无效")
        if "," in ssid:
            raise ValueError("SSID 不能包含逗号")
        ip = _norm_ip(ip)
        netmask = _norm_ip(netmask)
        gateway = _norm_ip(gateway)

        replies: list[str] = []
        notify_q: asyncio.Queue[str] = asyncio.Queue()

        def on_notify(_handle: int, data: bytearray) -> None:
            try:
                text = bytes(data).decode("utf-8", errors="replace")
            except Exception:
                text = repr(bytes(data))
            for line in text.replace("\r", "\n").split("\n"):
                line = line.strip()
                if line:
                    replies.append(line)
                    notify_q.put_nowait(line)
                    self._push("info", f"<- {line}")

        async def write_line(client: BleakClient, line: str) -> None:
            payload = (line.rstrip("\n") + "\n").encode("utf-8")
            self._push("info", f"-> {line}")
            await client.write_gatt_char(NUS_RX, payload, response=False)

        async def wait_prefix(prefix: str, timeout: float) -> str:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                remain = deadline - time.monotonic()
                try:
                    line = await asyncio.wait_for(notify_q.get(), timeout=max(0.05, remain))
                except asyncio.TimeoutError:
                    break
                if line.startswith("ERR"):
                    raise RuntimeError(line)
                if line.startswith(prefix):
                    return line
            raise TimeoutError(f"等待设备回复超时（期望 {prefix}*）")

        async with self._lock:
            self._busy = True
            try:
                addr = (address or "").strip()
                if not addr:
                    self._push("info", "未指定地址，先扫描…")
                    found = await BleakScanner.discover(timeout=scan_timeout, return_adv=True)
                    for a, (dev, adv) in found.items():
                        name = dev.name or (adv.local_name if adv else None) or ""
                        if name and DEVICE_NAME.lower() in name.lower():
                            addr = a
                            break
                    if not addr:
                        raise RuntimeError(f"未扫描到 {DEVICE_NAME}，请先长按 BOOT 进入配网")
                    self._push("info", f"选用 {addr}")

                self._push("info", f"连接 {addr} …")
                async with BleakClient(addr, timeout=20.0) as client:
                    if not client.is_connected:
                        raise RuntimeError("BLE 连接失败")
                    self._push("info", "已连接，订阅通知")
                    await client.start_notify(NUS_TX, on_notify)
                    await asyncio.sleep(0.3)

                    try:
                        while True:
                            await asyncio.wait_for(notify_q.get(), timeout=0.5)
                    except asyncio.TimeoutError:
                        pass

                    await write_line(client, f"WIFI:{ssid},{password}")
                    await wait_prefix("OK wifi", reply_timeout)

                    await write_line(client, f"HOST:{host}:{int(port)}")
                    await wait_prefix("OK host", reply_timeout)

                    if ip:
                        await write_line(client, f"IP:{ip}")
                        await wait_prefix("OK ip", reply_timeout)
                    if netmask:
                        await write_line(client, f"MASK:{netmask}")
                        await wait_prefix("OK mask", reply_timeout)
                    if gateway:
                        await write_line(client, f"GW:{gateway}")
                        await wait_prefix("OK gw", reply_timeout)

                    await write_line(client, "GET")
                    try:
                        await wait_prefix("OK ssid=", reply_timeout)
                    except TimeoutError:
                        self._push("warn", "GET 无回复，继续 APPLY")

                    await write_line(client, "APPLY")
                    apply_line = await wait_prefix("OK apply", reply_timeout)

                    await asyncio.sleep(0.4)
                    try:
                        await client.stop_notify(NUS_TX)
                    except Exception:
                        pass

                self._push("info", "配网完成，设备将重连 WiFi/WS")
                return {
                    "ok": True,
                    "address": addr,
                    "replies": replies,
                    "apply": apply_line,
                    "host": f"{host}:{int(port)}",
                    "ssid": ssid,
                    "ip": ip or None,
                    "netmask": netmask or None,
                    "gateway": gateway or None,
                }
            except Exception as exc:
                self._push("error", str(exc))
                raise
            finally:
                self._busy = False


ble_prov = BleProvService()
