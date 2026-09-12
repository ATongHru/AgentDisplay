#!/usr/bin/env python3
"""Generate docs/flash_usage.svg, docs/psram_usage.svg, docs/dram_usage.svg.

布局：外框长方形 + 下方彩色子长方形（按占用从高到低），条内/右侧注明文件。
"""

from __future__ import annotations

from pathlib import Path

FLASH_TOTAL = 16 * 1024 * 1024
PSRAM_TOTAL = 8 * 1024 * 1024
DRAM_SHOW = 512 * 1024
OUT = Path(__file__).resolve().parent.parent / "docs"


def fmt(n: int) -> str:
    if n >= 1024 * 1024:
        return f"{n / (1024 * 1024):.2f} MB"
    if n >= 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n} B"


# name, offset, capacity, used, color_used, color_free, file
FLASH = [
    ("bootloader", 0x0, 0x8000, 22368, "#3B82F6", "#1E3A5F", "build/bootloader/bootloader.bin"),
    ("part-table", 0x8000, 0x1000, 3072, "#818CF8", "#312E81", "build/partition_table/partition-table.bin"),
    ("nvs", 0x9000, 0x5000, 0, "#64748B", "#334155", "NVS 运行时（agent_cfg）"),
    ("otadata", 0xE000, 0x2000, 8192, "#A78BFA", "#4C1D95", "build/ota_data_initial.bin"),
    ("app0", 0x10000, 0x300000, 1890880, "#22C55E", "#14532D", "build/esp32s3_agent_display.bin（含CJK）"),
    ("spiffs", 0x310000, 0xE0000, 0, "#475569", "#1E293B", "（空）遗留"),
    ("coredump", 0x3F0000, 0x10000, 0, "#475569", "#1E293B", "（空）崩溃时写"),
    ("voice_font", 0x400000, 0x200000, 0, "#475569", "#1E293B", "（空）旧字库槽"),
    ("animations", 0x600000, 0x600000, 3557266, "#F59E0B", "#78350F", "firmware/data/animations.bin"),
    ("storage", 0xC00000, 0x400000, 0, "#475569", "#1E293B", "（空）预留 4MB"),
]

PSRAM = [
    ("录音 PCM", 320 * 1024, "#EF4444", "main/audio.c → VOICE_MAX_RECORD_BYTES"),
    ("播放 ring", 128 * 1024, "#FB923C", "main/audio.c → VOICE_PLAY_RING_BYTES"),
    ("LVGL partial×2", 240 * 20 * 2 * 2, "#22D3EE", "main/display.c（优先 PSRAM）"),
]

DRAM = [
    ("表情双缓冲 BSS", 90 * 90 * 2 * 2, "#E879F9", "main/ui.c → frame_buffer_a/b"),
    ("SPIRAM 预留片内", 32768, "#C084FC", "sdkconfig SPIRAM_MALLOC_RESERVE_INTERNAL"),
    ("任务栈等（估）", 80 * 1024, "#A78BFA", "FreeRTOS 各任务栈"),
]


def _esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _header(W: int, H: int, title: str, subtitle: str) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
        f'<rect width="{W}" height="{H}" rx="16" fill="#0B1220"/>',
        f'<text x="28" y="36" fill="#F8FAFC" font-size="22" font-family="Segoe UI,Microsoft YaHei,sans-serif" font-weight="700">{_esc(title)}</text>',
        f'<text x="28" y="58" fill="#94A3B8" font-size="13" font-family="Segoe UI,Microsoft YaHei,sans-serif">{_esc(subtitle)}</text>',
    ]


def _map_bar(parts: list[tuple[str, int, int, str, str]], total: int, y: int, h: int, inner: float, pad: float) -> list[str]:
    """parts: name, cap, used, color_used, color_free"""
    L: list[str] = []
    x = pad
    for name, cap, used, cu, cf in parts:
        w = max(2.0, inner * (cap / total))
        L.append(f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="{h}" fill="{cf}" stroke="#020617" stroke-width="1"/>')
        if used > 0:
            uw = max(1.0, w * min(1.0, used / cap))
            L.append(f'<rect x="{x:.1f}" y="{y}" width="{uw:.1f}" height="{h}" fill="{cu}"/>')
        if w >= 56:
            L.append(
                f'<text x="{x + w / 2:.1f}" y="{y + h / 2 + 5:.1f}" fill="#F8FAFC" font-size="12" '
                f'text-anchor="middle" font-family="Segoe UI,Microsoft YaHei,sans-serif">{_esc(name)}</text>'
            )
        x += w
    return L


def _ranked_rows(
    items: list[tuple[str, int, str, str]],
    total: int,
    y0: int,
    row_h: int,
    gap: int,
    canvas_w: float,
    pad: float,
    unit_label: str,
) -> tuple[list[str], int]:
    """Vertical stack of sub-rectangles (desc). Color width ∝ size/total; name+file always readable."""
    L: list[str] = []
    y = y0
    ranked = sorted(items, key=lambda x: -x[1])
    # left: color bar track; right: text column
    track_w = canvas_w * 0.42
    text_x = pad + track_w + 18
    for i, (name, size, color, file_) in enumerate(ranked, 1):
        L.append(f'<rect x="{pad:.1f}" y="{y}" width="{track_w:.1f}" height="{row_h}" rx="8" fill="#1E293B" stroke="#475569" stroke-width="1.5"/>')
        fill_w = max(10.0, track_w * (size / total))
        L.append(f'<rect x="{pad:.1f}" y="{y}" width="{fill_w:.1f}" height="{row_h}" rx="8" fill="{color}"/>')
        pct = size * 100 / total
        L.append(
            f'<text x="{text_x:.1f}" y="{y + 22}" fill="#F8FAFC" font-size="15" font-weight="700" '
            f'font-family="Segoe UI,Microsoft YaHei,sans-serif">{i}. {_esc(name)}  ·  {fmt(size)}  ({pct:.2f}% {unit_label})</text>'
        )
        L.append(
            f'<text x="{text_x:.1f}" y="{y + 42}" fill="#94A3B8" font-size="12" '
            f'font-family="Consolas,Segoe UI,Microsoft YaHei,monospace">{_esc(file_)}</text>'
        )
        y += row_h + gap
    return L, y


def write_flash() -> None:
    W, pad = 1000, 28
    used_items = [(n, u, cu, f) for n, _o, _c, u, cu, _cf, f in FLASH if u > 0]
    used_sum = sum(s for _, s, _, _ in used_items)
    empty = sum(c for _, _, c, u, _, _, _ in FLASH if u == 0)
    slack = sum(c - u for _, _, c, u, _, _, _ in FLASH if 0 < u < c)
    free_total = empty + slack

    n = len(used_items)
    H = 120 + 72 + n * (56 + 14) + 70
    L = _header(
        W,
        H,
        "Flash 16 MB — 占用长方形图",
        "上：按地址分区（亮=已烧录）；下：实际占用子长方形，由高到低，右侧注明文件",
    )
    inner = W - pad * 2
    map_y, map_h = 78, 72
    L.append(f'<rect x="{pad}" y="{map_y}" width="{inner}" height="{map_h}" rx="8" fill="#020617" stroke="#334155" stroke-width="2"/>')
    parts = [(n, c, u, cu, cf) for n, _o, c, u, cu, cf, _f in FLASH]
    L += _map_bar(parts, FLASH_TOTAL, map_y, map_h, inner, float(pad))
    L.append(f'<text x="{pad}" y="{map_y + map_h + 22}" fill="#64748B" font-size="12" font-family="Consolas,monospace">0x000000</text>')
    L.append(
        f'<text x="{W - pad}" y="{map_y + map_h + 22}" fill="#64748B" font-size="12" text-anchor="end" '
        f'font-family="Consolas,monospace">0x1000000</text>'
    )

    y0 = map_y + map_h + 48
    L.append(
        f'<text x="{pad}" y="{y0}" fill="#F8FAFC" font-size="16" font-family="Segoe UI,Microsoft YaHei,sans-serif" '
        f'font-weight="700">已烧录占用（左侧色条宽度 = 相对整片 Flash；由高到低）</text>'
    )
    rows, y = _ranked_rows(used_items, FLASH_TOTAL, y0 + 16, 52, 14, inner, float(pad), "Flash")
    L += rows
    L.append(
        f'<text x="{pad}" y="{y + 8}" fill="#94A3B8" font-size="13" font-family="Segoe UI,Microsoft YaHei,sans-serif">'
        f"已烧录合计 {fmt(used_sum)}；其余约 {fmt(free_total)}（{free_total * 100 / FLASH_TOTAL:.1f}%）为空闲/未写。</text>"
    )
    L.append("</svg>")
    h = y + 40
    out = "\n".join(L).replace(f'height="{H}"', f'height="{h}"', 1).replace(f'viewBox="0 0 {W} {H}"', f'viewBox="0 0 {W} {h}"', 1)
    out = out.replace(f'<rect width="{W}" height="{H}"', f'<rect width="{W}" height="{h}"', 1)
    (OUT / "flash_usage.svg").write_text(out, encoding="utf-8")


def write_psram() -> None:
    W, pad = 1000, 28
    used = list(PSRAM)
    used_sum = sum(s for _, s, _, _ in used)
    free = PSRAM_TOTAL - used_sum
    n = len(used)
    H = 120 + 64 + n * (56 + 14) + 80
    L = _header(
        W,
        H,
        "PSRAM 8 MB — 占用长方形图",
        "上：相对整片 8MB 真实比例；下：已知缓冲子长方形，由高到低，右侧注明源码位置",
    )
    inner = W - pad * 2
    map_y, map_h = 78, 64
    L.append(f'<rect x="{pad}" y="{map_y}" width="{inner}" height="{map_h}" rx="8" fill="#020617" stroke="#334155" stroke-width="2"/>')
    x = float(pad)
    for _n, size, color, _f in sorted(used, key=lambda t: -t[1]):
        w = max(6.0, inner * (size / PSRAM_TOTAL))
        L.append(f'<rect x="{x:.1f}" y="{map_y}" width="{w:.1f}" height="{map_h}" fill="{color}"/>')
        x += w
    fw = inner * (free / PSRAM_TOTAL)
    L.append(f'<rect x="{x:.1f}" y="{map_y}" width="{fw:.1f}" height="{map_h}" fill="#1E293B"/>')
    L.append(
        f'<text x="{x + fw / 2:.1f}" y="{map_y + map_h / 2 + 5:.1f}" fill="#94A3B8" font-size="14" text-anchor="middle" '
        f'font-family="Segoe UI,Microsoft YaHei,sans-serif">空闲堆 ≈ {fmt(free)}</text>'
    )

    y0 = map_y + map_h + 40
    L.append(
        f'<text x="{pad}" y="{y0}" fill="#F8FAFC" font-size="16" font-family="Segoe UI,Microsoft YaHei,sans-serif" '
        f'font-weight="700">已知缓冲（左侧色条宽度 = 相对整片 PSRAM；由高到低）</text>'
    )
    rows, y = _ranked_rows(used, PSRAM_TOTAL, y0 + 16, 52, 14, inner, float(pad), "PSRAM")
    L += rows
    L.append(
        f'<text x="{pad}" y="{y + 8}" fill="#94A3B8" font-size="13" font-family="Segoe UI,Microsoft YaHei,sans-serif">'
        f"已知缓冲合计 {fmt(used_sum)}（{used_sum * 100 / PSRAM_TOTAL:.2f}%）。表情双缓冲约 32KB 在片内 DRAM，不在此图。</text>"
    )
    L.append("</svg>")
    h = y + 40
    out = "\n".join(L).replace(f'height="{H}"', f'height="{h}"', 1).replace(f'viewBox="0 0 {W} {H}"', f'viewBox="0 0 {W} {h}"', 1)
    out = out.replace(f'<rect width="{W}" height="{H}"', f'<rect width="{W}" height="{h}"', 1)
    (OUT / "psram_usage.svg").write_text(out, encoding="utf-8")


def write_dram() -> None:
    W, pad = 1000, 28
    used = list(DRAM)
    n = len(used)
    H = 100 + n * (56 + 14) + 50
    L = _header(
        W,
        H,
        "片内 DRAM（约 512KB 可用池示意）",
        "左侧色条宽度相对 512KB 示意池；BLE 前需 wifi_deinit 腾连续块",
    )
    inner = W - pad * 2
    rows, y = _ranked_rows(used, DRAM_SHOW, 78, 52, 14, inner, float(pad), "示意池")
    L += rows
    L.append("</svg>")
    h = y + 28
    out = "\n".join(L).replace(f'height="{H}"', f'height="{h}"', 1).replace(f'viewBox="0 0 {W} {H}"', f'viewBox="0 0 {W} {h}"', 1)
    out = out.replace(f'<rect width="{W}" height="{H}"', f'<rect width="{W}" height="{h}"', 1)
    (OUT / "dram_usage.svg").write_text(out, encoding="utf-8")


if __name__ == "__main__":
    write_flash()
    write_psram()
    write_dram()
    print("wrote", OUT / "flash_usage.svg")
    print("wrote", OUT / "psram_usage.svg")
    print("wrote", OUT / "dram_usage.svg")
