#!/usr/bin/env python3
"""Generate docs/flash_usage.svg, docs/psram_usage.svg, docs/dram_usage.svg."""

from __future__ import annotations

from pathlib import Path

FLASH_TOTAL = 16 * 1024 * 1024
PSRAM_TOTAL = 8 * 1024 * 1024
DRAM_SHOW = 512 * 1024
OUT = Path(__file__).resolve().parent.parent / "docs"

APP_BIN = 1_442_336
FONT_BIN = 914_036
ANIM_BIN = 4_619_137
BOOT_BIN = 22_368
PART_BIN = 3_072
OTA_BIN = 8_192

VOICE_RECORD = 16_000 * 2 * 20
VOICE_PLAY_RING = 256 * 1024
LVGL_PARTIAL_X2 = 240 * 20 * 2 * 2
FACE_BSS = 90 * 90 * 2 * 2
SPIRAM_RESERVE_INTERNAL = 32_768
TASK_STACKS_EST = 80 * 1024


def fmt(n: int) -> str:
    if n >= 1024 * 1024:
        return f"{n / (1024 * 1024):.2f} MB"
    if n >= 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n} B"


FLASH = [
    ("bootloader", 0x0, 0x8000, BOOT_BIN, "#3B82F6", "#1E3A5F", "build/bootloader/bootloader.bin"),
    ("part-table", 0x8000, 0x1000, PART_BIN, "#818CF8", "#312E81", "build/partition_table/partition-table.bin"),
    ("nvs", 0x9000, 0x5000, 0, "#64748B", "#334155", "NVS 运行时（agent_cfg）"),
    ("otadata", 0xE000, 0x2000, OTA_BIN, "#A78BFA", "#4C1D95", "build/ota_data_initial.bin"),
    ("app0", 0x10000, 0x300000, APP_BIN, "#22C55E", "#14532D", "build/esp32s3_agent_display.bin"),
    ("spiffs", 0x310000, 0xE0000, 0, "#475569", "#1E293B", "（空）遗留"),
    ("coredump", 0x3F0000, 0x10000, 0, "#475569", "#1E293B", "（空）崩溃时写"),
    ("cjk_font", 0x400000, 0x200000, FONT_BIN, "#38BDF8", "#0C4A6E", "firmware/data/font_cjk_16.bin"),
    ("animations", 0x600000, 0x600000, ANIM_BIN, "#F59E0B", "#78350F", "firmware/data/animations.bin"),
    ("storage", 0xC00000, 0x400000, 0, "#475569", "#1E293B", "（空）预留 4MB"),
]

PSRAM = [
    ("CJK 字库运行时", FONT_BIN, "#38BDF8", "main/font_loader.c（lv_binfont 拷贝）"),
    ("录音 PCM", VOICE_RECORD, "#EF4444", "main/board_pins.h → VOICE_MAX_RECORD_BYTES"),
    ("播放 ring", VOICE_PLAY_RING, "#FB923C", "main/board_pins.h → VOICE_PLAY_RING_BYTES"),
    ("LVGL partial×2", LVGL_PARTIAL_X2, "#22D3EE", "main/display.c → PARTIAL_BUF_LINES=20"),
]

DRAM = [
    ("任务栈等（估）", TASK_STACKS_EST, "#A78BFA", "FreeRTOS 各任务栈"),
    ("SPIRAM 预留片内", SPIRAM_RESERVE_INTERNAL, "#C084FC", "sdkconfig SPIRAM_MALLOC_RESERVE_INTERNAL"),
    ("表情双缓冲 BSS", FACE_BSS, "#E879F9", "main/ui.c → frame_buffer_a/b"),
]


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def header(W: int, H: int, title: str, subtitle: str) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
        f'<rect width="{W}" height="{H}" rx="16" fill="#0B1220"/>',
        (
            f'<text x="28" y="36" fill="#F8FAFC" font-size="22" '
            f'font-family="Segoe UI,Microsoft YaHei,sans-serif" font-weight="700">{esc(title)}</text>'
        ),
        (
            f'<text x="28" y="58" fill="#94A3B8" font-size="13" '
            f'font-family="Segoe UI,Microsoft YaHei,sans-serif">{esc(subtitle)}</text>'
        ),
    ]


def map_bar(parts, total, y, h, inner, pad):
    lines = []
    x = pad
    for name, cap, used, cu, cf in parts:
        w = max(2.0, inner * (cap / total))
        lines.append(
            f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="{h}" fill="{cf}" '
            f'stroke="#020617" stroke-width="1"/>'
        )
        if used > 0:
            uw = max(1.0, w * min(1.0, used / cap))
            lines.append(f'<rect x="{x:.1f}" y="{y}" width="{uw:.1f}" height="{h}" fill="{cu}"/>')
        if w >= 56:
            lines.append(
                f'<text x="{x + w / 2:.1f}" y="{y + h / 2 + 5:.1f}" fill="#F8FAFC" font-size="12" '
                f'text-anchor="middle" font-family="Segoe UI,Microsoft YaHei,sans-serif">{esc(name)}</text>'
            )
        x += w
    return lines


def ranked_rows(items, total, y0, row_h, gap, canvas_w, pad, unit_label):
    lines = []
    y = y0
    ranked = sorted(items, key=lambda item: -item[1])
    track_w = canvas_w * 0.42
    text_x = pad + track_w + 18
    for i, (name, size, color, file_) in enumerate(ranked, 1):
        lines.append(
            f'<rect x="{pad:.1f}" y="{y}" width="{track_w:.1f}" height="{row_h}" rx="8" '
            f'fill="#1E293B" stroke="#475569" stroke-width="1.5"/>'
        )
        fill_w = max(10.0, track_w * (size / total))
        lines.append(
            f'<rect x="{pad:.1f}" y="{y}" width="{fill_w:.1f}" height="{row_h}" rx="8" fill="{color}"/>'
        )
        pct = size * 100 / total
        lines.append(
            f'<text x="{text_x:.1f}" y="{y + 22}" fill="#F8FAFC" font-size="15" font-weight="700" '
            f'font-family="Segoe UI,Microsoft YaHei,sans-serif">'
            f"{i}. {esc(name)}  ·  {fmt(size)}  ({pct:.2f}% {unit_label})</text>"
        )
        lines.append(
            f'<text x="{text_x:.1f}" y="{y + 42}" fill="#94A3B8" font-size="12" '
            f'font-family="Consolas,Segoe UI,Microsoft YaHei,monospace">{esc(file_)}</text>'
        )
        y += row_h + gap
    return lines, y


def fit(svg_lines, W, H, h):
    out = "\n".join(svg_lines)
    out = out.replace(f'height="{H}"', f'height="{h}"', 1)
    out = out.replace(f'viewBox="0 0 {W} {H}"', f'viewBox="0 0 {W} {h}"', 1)
    out = out.replace(f'<rect width="{W}" height="{H}"', f'<rect width="{W}" height="{h}"', 1)
    return out


def write_flash():
    W, pad = 1000, 28
    used_items = [(n, u, cu, f) for n, _o, _c, u, cu, _cf, f in FLASH if u > 0]
    used_sum = sum(s for _, s, _, _ in used_items)
    empty = sum(c for _, _, c, u, _, _, _ in FLASH if u == 0)
    slack = sum(c - u for _, _, c, u, _, _, _ in FLASH if 0 < u < c)
    free_total = empty + slack
    n = len(used_items)
    H = 120 + 72 + n * (56 + 14) + 70
    lines = header(
        W,
        H,
        "Flash 16 MB — 占用长方形图",
        "上：按地址分区（亮=已烧录）；下：实际占用子长方形，由高到低，右侧注明文件",
    )
    inner = W - pad * 2
    map_y, map_h = 78, 72
    lines.append(
        f'<rect x="{pad}" y="{map_y}" width="{inner}" height="{map_h}" rx="8" '
        f'fill="#020617" stroke="#334155" stroke-width="2"/>'
    )
    parts = [(n, c, u, cu, cf) for n, _o, c, u, cu, cf, _f in FLASH]
    lines += map_bar(parts, FLASH_TOTAL, map_y, map_h, inner, float(pad))
    lines.append(
        f'<text x="{pad}" y="{map_y + map_h + 22}" fill="#64748B" font-size="12" '
        f'font-family="Consolas,monospace">0x000000</text>'
    )
    lines.append(
        f'<text x="{W - pad}" y="{map_y + map_h + 22}" fill="#64748B" font-size="12" text-anchor="end" '
        f'font-family="Consolas,monospace">0x1000000</text>'
    )
    y0 = map_y + map_h + 48
    lines.append(
        f'<text x="{pad}" y="{y0}" fill="#F8FAFC" font-size="16" '
        f'font-family="Segoe UI,Microsoft YaHei,sans-serif" font-weight="700">'
        f"已烧录占用（左侧色条宽度 = 相对整片 Flash；由高到低）</text>"
    )
    rows, y = ranked_rows(used_items, FLASH_TOTAL, y0 + 16, 52, 14, inner, float(pad), "Flash")
    lines += rows
    lines.append(
        f'<text x="{pad}" y="{y + 8}" fill="#94A3B8" font-size="13" '
        f'font-family="Segoe UI,Microsoft YaHei,sans-serif">'
        f"已烧录合计 {fmt(used_sum)}；其余约 {fmt(free_total)}"
        f"（{free_total * 100 / FLASH_TOTAL:.1f}%）为空闲/未写。</text>"
    )
    lines.append("</svg>")
    (OUT / "flash_usage.svg").write_text(fit(lines, W, H, y + 40), encoding="utf-8")


def write_psram():
    W, pad = 1000, 28
    used = list(PSRAM)
    used_sum = sum(s for _, s, _, _ in used)
    free = PSRAM_TOTAL - used_sum
    n = len(used)
    H = 120 + 64 + n * (56 + 14) + 80
    lines = header(
        W,
        H,
        "PSRAM 8 MB — 占用长方形图",
        "上：相对整片 8MB 真实比例；下：已知缓冲子长方形，由高到低，右侧注明源码位置",
    )
    inner = W - pad * 2
    map_y, map_h = 78, 64
    lines.append(
        f'<rect x="{pad}" y="{map_y}" width="{inner}" height="{map_h}" rx="8" '
        f'fill="#020617" stroke="#334155" stroke-width="2"/>'
    )
    x = float(pad)
    for _n, size, color, _f in sorted(used, key=lambda t: -t[1]):
        w = max(6.0, inner * (size / PSRAM_TOTAL))
        lines.append(f'<rect x="{x:.1f}" y="{map_y}" width="{w:.1f}" height="{map_h}" fill="{color}"/>')
        x += w
    fw = inner * (free / PSRAM_TOTAL)
    lines.append(f'<rect x="{x:.1f}" y="{map_y}" width="{fw:.1f}" height="{map_h}" fill="#1E293B"/>')
    lines.append(
        f'<text x="{x + fw / 2:.1f}" y="{map_y + map_h / 2 + 5:.1f}" fill="#94A3B8" font-size="14" '
        f'text-anchor="middle" font-family="Segoe UI,Microsoft YaHei,sans-serif">'
        f"空闲堆 ≈ {fmt(free)}</text>"
    )
    y0 = map_y + map_h + 40
    lines.append(
        f'<text x="{pad}" y="{y0}" fill="#F8FAFC" font-size="16" '
        f'font-family="Segoe UI,Microsoft YaHei,sans-serif" font-weight="700">'
        f"已知缓冲（左侧色条宽度 = 相对整片 PSRAM；由高到低）</text>"
    )
    rows, y = ranked_rows(used, PSRAM_TOTAL, y0 + 16, 52, 14, inner, float(pad), "PSRAM")
    lines += rows
    lines.append(
        f'<text x="{pad}" y="{y + 8}" fill="#94A3B8" font-size="13" '
        f'font-family="Segoe UI,Microsoft YaHei,sans-serif">'
        f"已知缓冲合计 {fmt(used_sum)}（{used_sum * 100 / PSRAM_TOTAL:.2f}%）。"
        f"表情双缓冲约 {fmt(FACE_BSS)} 在片内 DRAM，不在此图。</text>"
    )
    lines.append("</svg>")
    (OUT / "psram_usage.svg").write_text(fit(lines, W, H, y + 40), encoding="utf-8")


def write_dram():
    W, pad = 1000, 28
    used = list(DRAM)
    n = len(used)
    H = 100 + n * (56 + 14) + 50
    lines = header(
        W,
        H,
        "片内 DRAM（约 512KB 可用池示意）",
        "左侧色条宽度相对 512KB 示意池；BLE 前需 wifi_deinit 腾连续块",
    )
    inner = W - pad * 2
    rows, y = ranked_rows(used, DRAM_SHOW, 78, 52, 14, inner, float(pad), "示意池")
    lines += rows
    lines.append("</svg>")
    (OUT / "dram_usage.svg").write_text(fit(lines, W, H, y + 28), encoding="utf-8")


if __name__ == "__main__":
    write_flash()
    write_psram()
    write_dram()
    print("wrote", OUT / "flash_usage.svg")
    print("wrote", OUT / "psram_usage.svg")
    print("wrote", OUT / "dram_usage.svg")
    flash_rank = sorted([(n, u) for n, _, _, u, *_ in FLASH if u > 0], key=lambda t: -t[1])
    print("Flash rank:", [f"{n}={fmt(u)}" for n, u in flash_rank])
    print("PSRAM rank:", [f"{n}={fmt(s)}" for n, s, *_ in sorted(PSRAM, key=lambda t: -t[1])])
