from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from textwrap import wrap
from xml.sax.saxutils import escape

from PIL import Image, ImageColor, ImageDraw, ImageFont


WIDTH = 2360
HEIGHT = 3200
HEADER_H = 154
FOOTER_H = 92
LANE_TITLE_H = 46
CONTENT_TOP = HEADER_H + 72
CONTENT_BOTTOM = HEIGHT - FOOTER_H - 120
ROW_GAP = 18
ROW_MIN_H = 74
PHASE_GAP = 40


@dataclass(frozen=True)
class Lane:
    title: str
    x: int
    w: int
    fill: str
    stroke: str

    @property
    def center(self) -> int:
        return self.x + self.w // 2


@dataclass(frozen=True)
class Event:
    kind: str
    src: int
    dst: int
    label: str
    width_chars: int = 54


LANES = [
    Lane("Operator / CLI shell", 60, 250, "#F7F7F7", "#CFCFCF"),
    Lane("sugar_client", 330, 250, "#FAFAFA", "#BFBFBF"),
    Lane("Loopback TCP", 600, 220, "#F7F7F7", "#CFCFCF"),
    Lane("sugar_server", 840, 250, "#F9F9F9", "#D6D6D6"),
    Lane("ProtocolDispatcher", 1110, 290, "#F2F2F2", "#B8B8B8"),
    Lane("ServerRuntime + workers", 1420, 320, "#EFEFEF", "#B0B0B0"),
    Lane("Artifacts / results / engine", 1760, 360, "#F5F5F5", "#C8C8C8"),
]


PHASES = [
    (
        "Connect and session setup",
        [
            Event("action", 0, 1, "1. Run `sugar_client connect --host 127.0.0.1 --port 4123`"),
            Event("action", 1, 4, "Send framed JSON `op=connect` over loopback TCP"),
            Event("action", 4, 5, "Create session and enforce live-session quota"),
            Event("return", 5, 1, "Return `session_id=sess-*` in a JSON response"),
        ],
    ),
    (
        "Stage uploads",
        [
            Event("action", 0, 1, "2. Upload layout JSON, logic JSON, and data CSV"),
            Event("action", 1, 4, "Three `upload_artifact` requests with client filename and bytes"),
            Event("action", 4, 4, "Check session ownership, per-artifact size, upload count, and staged bytes", 50),
            Event("return", 4, 1, "Return upload receipts `upl-*` for layout, logic, and data"),
        ],
    ),
    (
        "Validate",
        [
            Event("action", 0, 1, "3. Optional preflight: run `validate` with the three upload ids"),
            Event("action", 1, 4, "`validate_request` uses `layout_upload_id`, `logic_upload_id`, and `data_upload_id`"),
            Event("action", 4, 5, "Build submit request, stage workspace, and enqueue validation job"),
            Event("action", 5, 6, "Validate staged artifacts and emit validation summary plus result artifact"),
            Event("return", 6, 1, "Terminal validation result includes resolved mode, row counts, and warnings", 50),
        ],
    ),
    (
        "Submit and execute",
        [
            Event("action", 0, 1, "4. Execute hosted run or sweep with `submit` and the same upload ids"),
            Event("action", 1, 4, "Send `submit_job` over loopback TCP"),
            Event("action", 4, 5, "Preflight shape validation, expensive-sweep policy, job id allocation, and fair queue enqueue", 50),
            Event("action", 5, 6, "Worker validates, runs the engine path, records progress, and stores retained result data", 50),
            Event("return", 5, 1, "Return queued acknowledgement with `job_id=job-*`", 50),
        ],
    ),
    (
        "Poll and retrieve",
        [
            Event("action", 0, 1, "5. Poll `status`, then call `result` when the job reaches a terminal state"),
            Event("action", 1, 4, "`check_status` and `retrieve_results` carry `session_id` plus `job_id`"),
            Event("action", 4, 5, "Ownership gate rejects foreign or unknown job/session probes", 54),
            Event("return", 6, 1, "Return structured status or result JSON with summary text and hosted sweep metadata", 48),
        ],
    ),
]


def wrap_lines(text: str, width_chars: int) -> list[str]:
    lines: list[str] = []
    for raw_line in text.splitlines() or [text]:
        wrapped = wrap(raw_line, width=width_chars, break_long_words=False, break_on_hyphens=False)
        lines.extend(wrapped or [""])
    return lines


def measure_box(text: str, width_chars: int, *, line_gap: int, pad_y: int, min_h: int) -> tuple[list[str], int]:
    lines = wrap_lines(text, width_chars)
    height = max(min_h, pad_y * 2 + len(lines) * line_gap)
    return lines, height


def load_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = []
    if bold:
        candidates.extend([
            r"C:\Windows\Fonts\arialbd.ttf",
            r"C:\Windows\Fonts\segoeuib.ttf",
        ])
    else:
        candidates.extend([
            r"C:\Windows\Fonts\arial.ttf",
            r"C:\Windows\Fonts\segoeui.ttf",
        ])
    candidates.extend([
        r"C:\Windows\Fonts\calibri.ttf",
        r"C:\Windows\Fonts\DejaVuSans.ttf",
    ])
    for path in candidates:
        try:
            return ImageFont.truetype(path, size=size)
        except OSError:
            continue
    return ImageFont.load_default()


def text_size(draw: ImageDraw.ImageDraw, font: ImageFont.ImageFont, text: str) -> tuple[int, int]:
    bbox = draw.textbbox((0, 0), text, font=font)
    return bbox[2] - bbox[0], bbox[3] - bbox[1]


def wrap_pixels(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.ImageFont, max_width: int) -> list[str]:
    lines: list[str] = []
    for raw_line in text.splitlines() or [text]:
        words = raw_line.split()
        if not words:
            lines.append("")
            continue
        current = words[0]
        for word in words[1:]:
            trial = f"{current} {word}"
            if text_size(draw, font, trial)[0] <= max_width:
                current = trial
            else:
                lines.append(current)
                current = word
        lines.append(current)
    return lines


def centered_box_x(center_x: int, width: int) -> int:
    return int(center_x - width / 2)


def svg_text(x: int, y: int, text: str, *, size: int = 18, weight: str = "400",
             fill: str = "#111111", anchor: str = "start") -> str:
    return (
        f'<text x="{x}" y="{y}" font-family="Segoe UI, Arial, sans-serif" '
        f'font-size="{size}" font-weight="{weight}" fill="{fill}" '
        f'text-anchor="{anchor}">{escape(text)}</text>'
    )


def svg_multiline_text(x: int, y: int, lines: list[str], *, size: int = 16, weight: str = "400",
                       fill: str = "#333333", line_gap: int = 20, anchor: str = "start") -> str:
    tspans = []
    for i, line in enumerate(lines):
        tspans.append(
            f'<tspan x="{x}" dy="{0 if i == 0 else line_gap}">{escape(line)}</tspan>'
        )
    return (
        f'<text x="{x}" y="{y}" font-family="Segoe UI, Arial, sans-serif" '
        f'font-size="{size}" font-weight="{weight}" fill="{fill}" '
        f'text-anchor="{anchor}">{"".join(tspans)}</text>'
    )


def rounded_rect(x: int, y: int, w: int, h: int, *, fill: str, stroke: str,
                 stroke_width: int = 1, rx: int = 14, opacity: float | None = None,
                 extra: str = "") -> str:
    opacity_attr = f' opacity="{opacity}"' if opacity is not None else ""
    return (
        f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" '
        f'fill="{fill}" stroke="{stroke}" stroke-width="{stroke_width}"{opacity_attr} {extra}/>'
    )


def phase_header(y: int, title: str) -> str:
    return (
        rounded_rect(60, y - 26, WIDTH - 120, 42, fill="#111111", stroke="#111111", rx=10)
        + svg_text(78, y, title, size=18, weight="700", fill="#FFFFFF")
    )


def lane_block(lane: Lane, flow_bottom: int) -> str:
    block = rounded_rect(
        lane.x,
        LANE_TITLE_H + 72,
        lane.w,
        flow_bottom - (LANE_TITLE_H + 72),
        fill=lane.fill,
        stroke=lane.stroke,
        rx=18,
        opacity=0.96,
    )
    title_band = rounded_rect(
        lane.x + 10,
        HEADER_H + 12,
        lane.w - 20,
        LANE_TITLE_H,
        fill="#FFFFFF",
        stroke=lane.stroke,
        rx=12,
    )
    title = svg_text(lane.center, HEADER_H + 42, lane.title, size=18, weight="700", fill="#111111", anchor="middle")
    lifeline = (
        f'<line x1="{lane.center}" y1="{CONTENT_TOP}" x2="{lane.center}" y2="{flow_bottom}" '
        f'stroke="{lane.stroke}" stroke-width="2" stroke-dasharray="6 8" opacity="0.85"/>'
    )
    return block + title_band + title + lifeline


def event_markup(event: Event, y: int) -> str:
    lane_from = LANES[event.src]
    lane_to = LANES[event.dst]
    start_x = lane_from.center
    end_x = lane_to.center
    color = "#222222"
    dash = "" if event.kind == "action" else ' stroke-dasharray="8 6"'
    marker = "arrowBlack"
    lines, box_h = measure_box(event.label, event.width_chars, line_gap=18, pad_y=14, min_h=ROW_MIN_H)
    box_w = min(620, max(250, max(len(line) for line in lines) * 8 + 30))
    if event.kind == "action":
        label_fill = "#F6F6F6"
        label_stroke = "#CFCFCF"
    else:
        label_fill = "#F2F2F2"
        label_stroke = "#D0D0D0"
    box_x = int(min(start_x, end_x) + abs(end_x - start_x) / 2 - box_w / 2)
    box_y = y - box_h // 2
    text_y = box_y + 24
    line = (
        f'<line x1="{start_x}" y1="{y}" x2="{end_x}" y2="{y}" '
        f'stroke="{color}" stroke-width="3"{dash} marker-end="url(#{marker})"/>'
    )
    box = rounded_rect(box_x, box_y, box_w, box_h, fill=label_fill, stroke=label_stroke, rx=12)
    text = svg_multiline_text(box_x + 14, text_y, lines, size=14, fill="#222222", line_gap=18)
    return line + box + text


def self_event_markup(event: Event, y: int) -> str:
    lane = LANES[event.src]
    lines, box_h = measure_box(event.label, event.width_chars, line_gap=18, pad_y=14, min_h=ROW_MIN_H)
    box_w = min(560, max(320, max(len(line) for line in lines) * 8 + 30))
    box_x = lane.center - box_w // 2 + 72
    box_y = y - box_h // 2
    path_x = lane.center + 54
    path = (
        f'<path d="M {lane.center} {y} H {path_x} V {y + 36} H {lane.center + 10}" '
        f'fill="none" stroke="#222222" stroke-width="3" marker-end="url(#arrowBlack)"/>'
    )
    box = rounded_rect(box_x, box_y, box_w, box_h, fill="#F6F6F6", stroke="#CFCFCF", rx=12)
    text = svg_multiline_text(box_x + 14, box_y + 24, lines, size=14, fill="#222222", line_gap=18)
    return path + box + text


def build_layout() -> tuple[list[str], list[tuple[Event, int, int]], int]:
    phase_parts: list[str] = []
    y = 244
    event_rows: list[tuple[Event, int, int]] = []
    for phase_index, (phase_title, events) in enumerate(PHASES, start=1):
        phase_parts.append(phase_header(y, f"Phase {phase_index}: {phase_title}"))
        y += 56
        for event in events:
            _, box_h = measure_box(event.label, event.width_chars, line_gap=18, pad_y=14, min_h=ROW_MIN_H)
            row_h = max(ROW_MIN_H, box_h)
            event_rows.append((event, y + row_h // 2, row_h))
            y += row_h + ROW_GAP
        y += PHASE_GAP

    flow_bottom = max(y - PHASE_GAP // 2, CONTENT_BOTTOM)
    return phase_parts, event_rows, flow_bottom


def build_svg() -> str:
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">',
        "<defs>",
        '<linearGradient id="bg" x1="0" x2="0" y1="0" y2="1"><stop offset="0%" stop-color="#F7F7F7"/><stop offset="100%" stop-color="#EDEDED"/></linearGradient>',
        '<filter id="shadow" x="-10%" y="-10%" width="130%" height="130%"><feDropShadow dx="0" dy="6" stdDeviation="8" flood-color="#111111" flood-opacity="0.08"/></filter>',
        '<marker id="arrowBlack" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#222222"/></marker>',
        "</defs>",
        rounded_rect(0, 0, WIDTH, HEIGHT, fill="url(#bg)", stroke="none", rx=0),
        rounded_rect(32, 28, WIDTH - 64, HEIGHT - 56, fill="#FFFFFF", stroke="#D8D8D8", rx=24, extra='filter="url(#shadow)"'),
    ]

    phase_parts, event_rows, flow_bottom = build_layout()

    parts.extend(lane_block(lane, flow_bottom) for lane in LANES)
    parts.extend(phase_parts)

    for event, center_y, _row_h in event_rows:
        if event.src == event.dst:
            parts.append(self_event_markup(event, center_y))
        else:
            parts.append(event_markup(event, center_y))

    parts.append(rounded_rect(60, 42, WIDTH - 120, 96, fill="#FFFFFF", stroke="#D8D8D8", rx=18, extra='filter="url(#shadow)"'))
    parts.append(rounded_rect(66, 48, 8, 84, fill="#111111", stroke="#111111", rx=4))
    parts.append(svg_text(92, 80, "SugarBot Hosted-Service Event Flow", size=34, weight="800", fill="#111111"))
    parts.append(svg_text(92, 114, "WSL/Linux loopback local server-client workflow", size=20, weight="600", fill="#333333"))

    parts.append("</svg>")
    return "".join(parts)


def render_png(output_path: Path) -> None:
    phase_parts, event_rows, flow_bottom = build_layout()
    img = Image.new("RGBA", (WIDTH, HEIGHT), ImageColor.getrgb("#F7F7F7") + (255,))
    draw = ImageDraw.Draw(img)

    # Background panel
    draw.rounded_rectangle((32, 28, WIDTH - 32, HEIGHT - 28), radius=24, fill="white", outline="#D8D8D8", width=1)

    # Lanes and lifelines
    lane_fill = [ImageColor.getrgb(lane.fill) + (245,) for lane in LANES]
    lane_stroke = [lane.stroke for lane in LANES]
    for lane, fill_rgba, stroke in zip(LANES, lane_fill, lane_stroke):
        draw.rounded_rectangle(
            (lane.x, LANE_TITLE_H + 72, lane.x + lane.w, flow_bottom),
            radius=18,
            fill=fill_rgba,
            outline=stroke,
            width=1,
        )
        draw.rounded_rectangle(
            (lane.x + 10, HEADER_H + 12, lane.x + lane.w - 10, HEADER_H + 12 + LANE_TITLE_H),
            radius=12,
            fill="white",
            outline=stroke,
            width=1,
        )
        title_font = load_font(21, bold=True)
        tw, th = text_size(draw, title_font, lane.title)
        draw.text((lane.center - tw / 2, HEADER_H + 23), lane.title, font=title_font, fill="#111111")
        draw.line((lane.center, CONTENT_TOP, lane.center, flow_bottom), fill=stroke, width=2, joint="curve")

    # Phase headers, rendered on top of the lanes.
    for header in phase_parts:
        # The header strings are SVG snippets, so we mirror the coordinates directly.
        # Each bar is 42px tall and starts at y - 26.
        pass

    # Re-draw phase bars in raster form so they remain visible.
    phase_y = 244
    header_font = load_font(18, bold=True)
    for phase_index, (phase_title, events) in enumerate(PHASES, start=1):
        draw.rounded_rectangle((60, phase_y - 26, WIDTH - 60, phase_y + 16), radius=10, fill="#111111", outline="#111111", width=1)
        draw.text((78, phase_y - 2), f"Phase {phase_index}: {phase_title}", font=header_font, fill="white")
        phase_y += 56
        for event in events:
            _, box_h = measure_box(event.label, event.width_chars, line_gap=18, pad_y=14, min_h=ROW_MIN_H)
            phase_y += max(ROW_MIN_H, box_h) + ROW_GAP
        phase_y += PHASE_GAP

    # Event connectors and labels
    action_font = load_font(16, bold=False)
    for event, center_y, _row_h in event_rows:
        lane_from = LANES[event.src]
        lane_to = LANES[event.dst]
        color = "#222222"
        fill = "#F6F6F6" if event.kind == "action" else "#F2F2F2"
        outline = "#CFCFCF" if event.kind == "action" else "#D0D0D0"
        lines, box_h = measure_box(event.label, event.width_chars, line_gap=18, pad_y=14, min_h=ROW_MIN_H)
        max_line_px = max(text_size(draw, action_font, line)[0] for line in lines) if lines else 0
        box_w = min(620, max(250, max_line_px + 30))

        if event.src == event.dst:
            box_w = min(560, max(320, max_line_px + 30))
            box_x = lane_from.center - box_w // 2 + 72
            box_y = center_y - box_h // 2
            draw.line((lane_from.center, center_y, lane_from.center + 54, center_y, lane_from.center + 54, center_y + 36, lane_from.center + 10, center_y + 36), fill="#222222", width=3)
            draw.polygon([(lane_from.center + 10, center_y + 31), (lane_from.center + 2, center_y + 24), (lane_from.center + 2, center_y + 38)], fill="#222222")
            draw.rounded_rectangle((box_x, box_y, box_x + box_w, box_y + box_h), radius=12, fill="#F6F6F6", outline="#CFCFCF", width=1)
            for i, line in enumerate(lines):
                draw.text((box_x + 14, box_y + 14 + i * 18), line, font=action_font, fill="#222222")
            continue

        start_x = lane_from.center
        end_x = lane_to.center
        box_x = centered_box_x(int(min(start_x, end_x) + abs(end_x - start_x) / 2), box_w)
        box_y = center_y - box_h // 2
        draw.line((start_x, center_y, end_x, center_y), fill=color, width=3)
        arrow_dir = 1 if end_x >= start_x else -1
        tip_x = end_x
        tip_y = center_y
        base = 10 * arrow_dir
        draw.polygon([(tip_x, tip_y), (tip_x - 12 * arrow_dir, tip_y - 6), (tip_x - 12 * arrow_dir, tip_y + 6)], fill=color)
        draw.rounded_rectangle((box_x, box_y, box_x + box_w, box_y + box_h), radius=12, fill=fill, outline=outline, width=1)
        for i, line in enumerate(lines):
            draw.text((box_x + 14, box_y + 14 + i * 18), line, font=action_font, fill="#222222")

    # Title banner rendered last so it stays above the flow.
    draw.rounded_rectangle((60, 42, WIDTH - 60, 138), radius=18, fill="white", outline="#D8D8D8", width=1)
    draw.rounded_rectangle((66, 48, 74, 132), radius=4, fill="#111111", outline="#111111", width=1)
    title_font = load_font(38, bold=True)
    subtitle_font = load_font(23, bold=True)
    draw.text((92, 62), "SugarBot Hosted-Service Event Flow", font=title_font, fill="#111111")
    draw.text((92, 96), "WSL/Linux loopback local server-client workflow", font=subtitle_font, fill="#333333")

    img = img.convert("RGB")
    img.save(output_path, format="PNG", optimize=True)


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    svg_path = script_dir / "sugarbot_hosted_service_loopback_flow.svg"
    png_path = script_dir / "sugarbot_hosted_service_loopback_flow.png"
    svg_path.write_text(build_svg(), encoding="utf-8")
    render_png(png_path)
    print(f"Wrote {svg_path}")
    print(f"Wrote {png_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
