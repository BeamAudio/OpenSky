"""Shared binary spec for pre-baked regional weather state libraries (.nswx).

Matches protocol/weather_format.h bit-for-bit. States are solved offline by
builder/bake_weather.py and replayed (never re-solved) by the native runtime.
"""

from __future__ import annotations

import struct

NSWX_MAGIC = b"NSWX"
NSWX_VERSION = 1

GRID_W = 128
GRID_H = 96
LEVELS = 4
LEVEL_AGL_M = (10.0, 550.0, 1800.0, 4500.0)

SEASONS = ("WINTER", "SPRING", "SUMMER", "AUTUMN")
SLOTS = (
    "07:15 DAWN", "08:30 EARLY AM", "09:45 FAIR VFR", "11:00 LATE AM", "12:15 PRE-NOON",
    "13:30 NOON", "14:45 EARLY PM", "16:00 MID PM", "17:15 GOLDEN", "18:30 SUNSET",
    "SHORT NIGHT",
)
NIGHT_SLOT = 10
WEATHER_TYPES = (
    "PYRENEAN THERMAL", "EL CIERZO NW", "FOEHN SUR", "KATABATIC DAWN", "CONVECTIVE STORM",
)
STORM_TYPE = 4
NSLOT = len(SLOTS)
NTYPE = len(WEATHER_TYPES)
NSEASON = len(SEASONS)
STATE_COUNT = NSEASON * NSLOT * NTYPE

# Channel quantization ranges shared with the C++ reader.
CHAN_LO = (-45.0, -45.0, -14.0, -1.0)  # u, v, w, cloud_delta
CHAN_HI = (45.0, 45.0, 14.0, 1.0)

HEADER_FMT = "<4s5I4d4f8f48s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 152

STATE_FMT = "<4IQ16f32s"
STATE_SIZE = struct.calcsize(STATE_FMT)
assert STATE_SIZE == 120

# Scalar slots in each state entry.
S_CLOUD_COV = 0
S_PRECIP = 1
S_CLOUD_BASE_M = 2
S_CLOUD_THICK_M = 3
S_OAT_SL_C = 4
S_DEWPOINT_SL_C = 5
S_QNH_HPA = 6
S_WIND_DIR_DEG = 7
S_WIND_SPEED_MPS = 8
S_HAZE_MULT = 9
S_TURB_SCALE = 10
S_STORM_INT = 11
S_VISIBILITY_M = 12
S_WAVE_AMP = 13
S_SNOWLINE_M = 14
S_SPARE = 15


def state_index(season: int, slot: int, wtype: int) -> int:
    return (season * NSLOT + slot) * NTYPE + wtype


def season_from_month(month: int) -> int:
    if month in (12, 1, 2):
        return 0
    if month in (3, 4, 5):
        return 1
    if month in (6, 7, 8):
        return 2
    return 3


def encode_channel(value, channel: int) -> int:
    lo, hi = CHAN_LO[channel], CHAN_HI[channel]
    q = round((float(value) - lo) / (hi - lo) * 255.0)
    return max(0, min(255, int(q)))


def pack_header(domain: tuple[float, float, float, float], provenance: str) -> bytes:
    return struct.pack(
        HEADER_FMT,
        NSWX_MAGIC,
        NSWX_VERSION,
        STATE_COUNT,
        GRID_W,
        GRID_H,
        LEVELS,
        domain[0], domain[1], domain[2], domain[3],
        *LEVEL_AGL_M,
        *CHAN_LO, *CHAN_HI,
        provenance.encode("ascii", errors="ignore")[:47],
    )


def pack_state(season: int, slot: int, wtype: int, comp_size: int, offset: int,
               scalars: list[float], label: str) -> bytes:
    vals = list(scalars[:16]) + [0.0] * (16 - len(scalars[:16]))
    return struct.pack(
        STATE_FMT,
        season, slot, wtype, comp_size, offset,
        *vals,
        label.encode("ascii", errors="ignore")[:31],
    )
