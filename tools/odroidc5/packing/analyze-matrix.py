#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Hardkernel Co., Ltd.
"""Compare packing-matrix captures with explicit layouts and source coordinates.

Exit0: all four cells match the required original-coordinate reference.
Exit1: valid inputs fail that reference or the capture protocol.
Exit2: invalid/missing input, dependency, or output path.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

try:
    import numpy as np
except ImportError:
    np = None

CASES = ((8, 8), (8, 10), (10, 8), (10, 10))
FORMATS = {
    "XRGB8888": (0x34325258, 4),
    "BGR888": (0x34324742, 3),
    "XBGR2101010": (0x30334258, 4),
}
LAYOUTS10 = {
    "XRGB2101010": (20, 10, 0),
    "XBGR2101010": (0, 10, 20),
    "RGBX1010102": (22, 12, 2),
    "BGRX1010102": (2, 12, 22),
}
PHASES = ("identity_coordinates", "adjacent32_xor1_coordinates")
FNV_CACHE = {}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fnv1a64(data):
    digest = sha256(data)
    if digest not in FNV_CACHE:
        value = 14695981039346656037
        for byte in data:
            value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
        FNV_CACHE[digest] = value
    return FNV_CACHE[digest]


def fields(line):
    return dict(re.findall(r"(\w+)=([^ ]+)", line))


def fixture(bits, width, height):
    x, y = np.meshgrid(np.arange(width, dtype=np.uint32),
                       np.arange(height, dtype=np.uint32))
    mask = (1 << bits) - 1
    ramp = x & mask
    red = ramp.copy()
    green = np.full_like(x, 1 << (bits - 4))
    blue = np.full_like(x, 1 << (bits - 1))
    red[128:256] = 1 << (bits - 4)
    green[128:256] = ramp[128:256]
    red[256:384] = 1 << (bits - 4)
    green[256:384] = 1 << (bits - 1)
    blue[256:384] = ramp[256:384]
    red[384:512] = ramp[384:512]
    green[384:512] = ramp[384:512]
    blue[384:512] = ramp[384:512]
    red[512:640] = (x[512:640] + 3 * y[512:640]) & mask
    green[512:640] = (3 * x[512:640] + 5 * y[512:640]) & mask
    blue[512:640] = (7 * x[512:640] + 11 * y[512:640]) & mask
    red[640:] = np.where(x[640:] & 1, mask, 0)
    green[640:] = np.where(y[640:] & 1, mask, 0)
    blue[640:] = np.where(x[640:] & 32, mask, 0)
    return np.stack((red, green, blue), axis=2)


def source_words(rgb, bits):
    red, green, blue = (rgb[:, :, i] for i in range(3))
    if bits == 8:
        return (red << 16) | (green << 8) | blue
    return red | (green << 10) | (blue << 20)


def conversions(rgb, source_bits, target_bits):
    if source_bits == target_bits:
        return {"identity_depth": rgb}
    if source_bits == 8:
        return {"left_shift_2": rgb << 2,
                "bit_replication_8_to_10": (rgb << 2) | (rgb >> 6)}
    return {"truncate_2": rgb >> 2,
            "round_half_up_clamp": np.minimum((rgb + 2) >> 2, 255),
            "full_range_scale_round": (rgb * 255 + 511) // 1023}


def histogram(values):
    unique, counts = np.unique(values, return_counts=True)
    return {str(int(value)): int(count)
            for value, count in zip(unique, counts)}


def compare(observed, expected):
    delta = np.abs(observed.astype(np.int16) - expected.astype(np.int16))
    equal = delta == 0
    return {
        "all_components_exact": bool(np.all(equal)),
        "pixels_with_any_error": int(np.count_nonzero(np.any(~equal, axis=2))),
        "component_mae": np.mean(delta, axis=(0, 1)).tolist(),
        "component_max_error": np.max(delta, axis=(0, 1)).tolist(),
        "parity_region_all_exact": bool(np.all(equal[640:])),
        "parity_region_component_mismatches":
            np.count_nonzero(~equal[640:], axis=(0, 1)).tolist(),
    }


def parse_log(path):
    data = path.read_bytes()
    lines = data.decode("utf-8").splitlines()
    raw = {}
    captures = []
    sources = []
    results = []
    for line in lines:
        if line.startswith("RAW "):
            entry = fields(line)
            require("case" in entry and entry["case"] not in raw,
                    f"{path.name}: missing or duplicate RAW case")
            raw[entry["case"]] = entry
        elif line.startswith("CAPTURE_JSON "):
            captures.append(json.loads(line.split(" ", 1)[1]))
        elif line.startswith("SOURCE "):
            sources.append(fields(line))
        elif line.startswith("RESULT_JSON "):
            results.append(json.loads(line.split(" ", 1)[1]))
    require(len(captures) == 2 and len(sources) == len(results) == 1,
            f"{path.name}: need two captures and one SOURCE/RESULT")
    captures.sort(key=lambda item: item["target_slot"])
    require([item["target_slot"] for item in captures] == [0, 1],
            f"{path.name}: expected independent target slots0/1")
    routes = set()
    for item in captures:
        route = item.get("route")
        require(route in ("headless", "clone") and
                item["case"] == f"{route}-{item['target_slot']}",
                f"{path.name}: capture name/route/slot mismatch")
        routes.add(route)
    require(len(routes) == 1 and len({item["case"] for item in captures}) == 2,
            f"{path.name}: need two distinct captures on the same route")
    require(re.fullmatch(r"[0-9a-fA-F]{64}", sources[0].get("notes", "")) is not None and
            re.fullmatch(r"[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}",
                         sources[0].get("boot", "")) is not None,
            f"{path.name}: missing kernel-notes or boot identity")
    return raw, captures, sources[0], results[0], sha256(data)


def read_payload(path, metadata, format_name, geometry=None):
    fourcc, cpp = FORMATS[format_name]
    width, height, pitch, count, offset = (
        int(metadata[key], 0) for key in
        ("width", "height", "pitch", "bytes", "offset"))
    require(int(metadata["cpp"], 0) == cpp and
            int(metadata["storage_fourcc"], 0) == fourcc and
            metadata["storage_name"] == format_name,
            f"{path.name}: format/cpp mismatch")
    require(width >= 1024 and not width % 2 and height >= 641,
            f"{path.name}: fixture needs an even width>=1024 and height>=641")
    require(pitch >= width * cpp and not pitch % 64 and offset == pitch,
            f"{path.name}: invalid guarded pitch/offset")
    require(geometry is None or geometry == (width, height),
            f"{path.name}: geometry differs within matrix")
    require(metadata.get("guards_and_padding") == "1" and
            metadata.get("no_CPU_swizzle") == "1",
            f"{path.name}: missing successful guard/padding or raw-storage record")
    data = path.read_bytes()
    require(len(data) == count == pitch * height,
            f"{path.name}: payload length mismatch")
    require(fnv1a64(data) == int(metadata["raw_fnv1a64"], 16),
            f"{path.name}: RAW checksum mismatch")
    rows = np.frombuffer(data, dtype=np.uint8).reshape(height, pitch)
    require(bool(np.all(rows[:, width * cpp:] == 0xa5)),
            f"{path.name}: unexpected row padding")
    active = rows[:, :width * cpp].copy()
    return data, active, (width, height), pitch, offset


def analyze_cell(root, source_bits, target_bits, geometry):
    name = f"rgb{source_bits}-rgb{target_bits}"
    raw, captures, source_record, protocol, log_hash = parse_log(root / (name + ".log"))
    folder = root / name
    source_format = "XRGB8888" if source_bits == 8 else "XBGR2101010"
    target_format = "BGR888" if target_bits == 8 else "XBGR2101010"
    require(source_record.get("source_format") == source_format and
            source_record.get("target_format") == target_format,
            f"{name}: SOURCE format differs from matrix cell")
    require(all(source_record.get(key) == "1" for key in
                ("CTM_NULL", "GAMMA_NULL", "RGB8_wire_request", "no_CPU_swizzle", "immutable")),
            f"{name}: expected NULL color state, RGB8 carrier and unmodified raw capture")
    require(source_format in raw, f"{name}: missing source RAW metadata")
    source, source_active, geometry, source_pitch, _ = read_payload(
        folder / (source_format + ".raw"), raw[source_format], source_format, geometry)
    width, height = geometry
    expected = fixture(source_bits, width, height)
    words = source_words(expected, source_bits)
    source_actual = source_active.view("<u4").reshape(height, width)
    require(bool(np.array_equal(source_actual, words)),
            f"{name}: source does not match the parity matrix fixture")
    payloads = []
    active = []
    metadata = []
    for capture in captures:
        label = capture["case"]
        require(label in raw, f"{name}: missing RAW record for {label}")
        data, pixels, _, pitch, offset = read_payload(
            folder / (label + ".raw"), raw[label], target_format, geometry)
        payloads.append(data)
        active.append(pixels)
        metadata.append({"case": label, "cpp": FORMATS[target_format][1],
                         "pitch": pitch, "offset": offset, "sha256": sha256(data)})
    repeat = payloads[0] == payloads[1]
    layouts = {}
    if target_bits == 8:
        rgb = active[0].reshape(height, width, 3).astype(np.uint32)
        layouts = {"BGR888": rgb, "RGB888": rgb[:, :, ::-1]}
    else:
        packed = active[0].view("<u4").reshape(height, width)
        for layout, shifts in LAYOUTS10.items():
            layouts[layout] = np.stack([(packed >> shift) & 1023
                                        for shift in shifts], axis=2)
    hypotheses = []
    for layout, observed in layouts.items():
        for conversion, reference in conversions(expected, source_bits, target_bits).items():
            for phase in PHASES:
                expected_phase = reference if phase == PHASES[0] else reference[:, np.arange(width) ^ 1]
                hypotheses.append({"layout": layout, "conversion": conversion,
                                   "phase": phase, **compare(observed, expected_phase)})
    exact = [{key: item[key] for key in ("layout", "conversion", "phase")}
             for item in hypotheses if item["all_components_exact"]]
    required_conversion = ("identity_depth" if source_bits == target_bits else
                           "left_shift_2" if source_bits == 8 else "round_half_up_clamp")
    required = [{"layout": target_format, "conversion": required_conversion,
                 "phase": PHASES[0]}]
    protocol_ok = (protocol.get("status") == "PASS" and
                   protocol.get("captures") == 2 and protocol.get("TEST_ONLY") == 2 and
                   protocol.get("standard_writeback") is True and
                   protocol.get("source_immutable") is True and
                   protocol.get("restore_verified") is True and
                   protocol.get("resources_released") is True and
                   all(item.get(key) is True for item in captures for key in
                       ("writeback_fence", "crtc_fence_event", "guard_rows", "source_immutable")))
    observed = layouts[target_format]
    entry = {
        "source_bits": source_bits, "target_bits": target_bits,
        "width": width, "height": height, "source_pitch": source_pitch,
        "source_fixture_all_words_exact": True, "source_sha256": sha256(source),
        "source_unused_bits_zero": bool(np.all(source_actual >> (24 if source_bits == 8 else 30) == 0)),
        "captures": metadata, "root_log_sha256": log_hash,
        "capture_protocol_pass": protocol_ok, "capture_protocol": protocol,
        "raw_checksums_verified": True, "capture_names_routes_slots_verified": True,
        "source_identity": {key: source_record.get(key) for key in ("notes", "boot")},
        "repeat_byte_identical": repeat, "explicit_hypotheses": hypotheses,
        "exact_hypotheses": exact, "required_identity_hypothesis": required,
        "required_identity_match": exact == required,
        "isolated_output_distinct_codes":
            [int(len(np.unique(observed[channel * 128, :1 << source_bits, channel])))
             for channel in range(3)],
        "isolated_output_lowbits":
            [histogram(observed[channel * 128, :1 << source_bits, channel] & 3)
             for channel in range(3)],
        "parity_row640_first8_pixels": observed[640, :8].tolist(),
        "active_word_identity_RGB10_to_RGB10":
            bool(np.array_equal(packed, source_actual)) if source_bits == target_bits == 10 else None,
    }
    if target_bits == 10:
        entry["unused_top2_bits_histogram"] = histogram(packed >> 30)
    return entry, geometry


def analyze(root):
    cases = {}
    geometry = None
    for source_bits, target_bits in CASES:
        entry, geometry = analyze_cell(root, source_bits, target_bits, geometry)
        cases[f"rgb{source_bits}-rgb{target_bits}"] = entry
    identities = {(item["source_identity"]["notes"].lower(),
                   item["source_identity"]["boot"].lower()) for item in cases.values()}
    require(len(identities) == 1, "matrix combines different kernel-notes or boot identities")
    passed = all(item["required_identity_match"] and item["repeat_byte_identical"] and
                 item["capture_protocol_pass"] for item in cases.values())
    rgb10 = cases["rgb10-rgb10"]
    precision = (passed and rgb10["active_word_identity_RGB10_to_RGB10"] and
                 rgb10["isolated_output_distinct_codes"] == [1024, 1024, 1024] and
                 all(set(hist) == {"0", "1", "2", "3"} for hist in rgb10["isolated_output_lowbits"]))
    return {
        "schema_version": 1,
        "fixture": "packing-matrix isolated R/G/B ramps, neutral, coordinates and y>=640 parity",
        "geometry": {"width": geometry[0], "height": geometry[1]},
        "cases": cases,
        "exact_coordinate_verdict": "PASS" if passed else "FAIL",
        "RGB10_storage_qualified_in_test_scope": bool(precision),
        "interpretation": "Only exact prescribed layouts/conversions and original coordinates qualify. Adjacent-pair hypotheses are diagnostic; no best fit or CPU correction is accepted.",
        "scope": "Actual supplied matrix mode, linear opaque primary, NULL CTM/gamma and recorded successful capture protocol. Not HDR, optical precision, other modes, modifiers, alpha or arbitrary composition.",
        "numpy_version": np.__version__,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-root", required=True, type=Path,
                        help="Directory containing four rgbN-rgbM folders and logs")
    parser.add_argument("--output", required=True, type=Path, help="New JSON report")
    args = parser.parse_args()
    if np is None:
        parser.error("NumPy is required")
    try:
        require(args.capture_root.is_dir(), "capture-root is not a directory")
        require(not args.output.exists(), "output must not already exist")
        report = analyze(args.capture_root)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as output:
            json.dump(report, output, indent=2)
            output.write("\n")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"analyze-matrix: {error}\n")
    print(json.dumps({"report": str(args.output),
                      "exact_coordinate_verdict": report["exact_coordinate_verdict"],
                      "RGB10_storage_qualified_in_test_scope":
                          report["RGB10_storage_qualified_in_test_scope"]}))
    return 0 if report["exact_coordinate_verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
