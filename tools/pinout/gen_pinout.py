# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Generate the package pinout (JSON) from the STM32CubeMX database.

The output lists the pins of the package by position: name as in CubeMX
("PC14-OSC32_IN"), type ("I/O", "Power", "MonoIO", "Reset"...) and the GPIO
name ("PC14"), null for a pin that is not a GPIO. It also maps the analog
signals to their GPIO ("ADC3_IN1": "PB1", "DAC1_OUT1": "PA4"); the internal
channels (temperature, VREFINT...) have no pin. Used by the web UI
(tools/bench/ui) to draw the package and name the pins of the channels.

Example (from the project root):
    uv run tools/pinout/gen_pinout.py -o tools/bench/ui/src/data/pinout.json
"""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

DEFAULT_XML = (Path.home() / "STM32CubeMX/db/mcu"
               / "STM32G474R(B-C-E)Tx.xml")


def local_name(tag: str) -> str:
    """Tag without its XML namespace."""
    return tag.rsplit("}", 1)[-1]


def parse(xml_path: Path) -> dict:
    root = ET.parse(xml_path).getroot()
    pins = []
    analog = {}
    for pin in root:
        if local_name(pin.tag) != "Pin":
            continue
        name = pin.get("Name")
        m = re.match(r"P[A-Z]\d+", name)
        if m:
            for signal in pin:
                sig = signal.get("Name", "")
                if (local_name(signal.tag) == "Signal"
                        and re.fullmatch(r"ADC\d+_IN\d+|DAC\d+_OUT\d+", sig)):
                    analog[sig] = m.group(0)
        pins.append({
            "position": int(pin.get("Position")),
            "name": name,
            "type": pin.get("Type"),
            "gpio": m.group(0) if m else None,
        })
    pins.sort(key=lambda p: p["position"])
    if [p["position"] for p in pins] != list(range(1, len(pins) + 1)):
        sys.exit("pin positions are not 1..N")
    return {
        "generated_by": "tools/pinout/gen_pinout.py",
        "source": xml_path.name,
        "mcu": root.get("RefName"),
        "package": root.get("Package"),
        "pins": pins,
        "analog": dict(sorted(analog.items())),
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("xml", nargs="?", type=Path, default=DEFAULT_XML,
                   help=f"CubeMX MCU file (default: {DEFAULT_XML})")
    p.add_argument("-o", "--output", type=Path,
                   help="output JSON file (default: standard output)")
    args = p.parse_args()

    text = json.dumps(parse(args.xml), indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
