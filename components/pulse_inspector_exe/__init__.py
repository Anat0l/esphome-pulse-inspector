"""Necta Executive-line decoder for PulseInspector.

Subscribes to two channels of a parent ``pulse_inspector`` and
decodes the edges as 8E1 UART per the MEI Protocol A specification:
9600 baud, 1 start + 8 data + 1 even-parity + 1 stop, LSB first.
The 9th bit of every byte is verified against even parity; a separate
``parity_errors`` counter is exposed and parity-broken bytes are
suffixed with ``p`` in the hex log. We physically use the same
edge-driven decoder as the MDB component because the wire layout
(11 bit-times per byte) is identical -- only the semantics of the
9th bit differ.

Higher-level semantics depend on the VMC manufacturer. The component
exposes:

- raw frames and counters (``on_master_frame`` / ``on_slave_frame``);
- typed YAML triggers (``on_vend_complete``, ``on_credit_change``,
  ``on_vmc_status``, …);
- HA sensors, binary sensors, and text sensors for credit, vend totals,
  and VMC state.

See ``components/pulse_inspector_exe/README.md`` for the full schema.
"""

import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_TRIGGER_ID

from esphome.components.pulse_inspector import PulseInspector

CODEOWNERS = ["@anat0l"]
DEPENDENCIES = ["esp32", "pulse_inspector"]
MULTI_CONF = True

CONF_INSPECTOR_ID = "inspector_id"
# Channel role names follow the MEI Protocol A spec terminology:
# "executive" is the master that sources commands (e.g. 0x31 STATUS,
# 0x32 CREDIT); "peripheral" is the slave that replies (VMC, ASU, CPP).
# We deliberately do NOT call these "tx" / "rx" because those names are
# ambiguous: the same wire is "TX" from the VMC's point of view but
# "RX" from the executive's, and historically users kept getting them
# swapped in YAML.
CONF_EXECUTIVE_CHANNEL = "executive_channel"
CONF_PERIPHERAL_CHANNEL = "peripheral_channel"
CONF_BAUD = "baud"
CONF_PARITY = "parity"
CONF_INTER_BYTE_TIMEOUT = "inter_byte_timeout"
CONF_LOG_RAW_FRAMES = "log_raw_frames"
CONF_SUPPRESS_IDLE_POLLS = "suppress_idle_polls"
CONF_SHOW_PARITY_IN_HEX = "show_parity_in_hex"
CONF_IDLE_MASTER_FRAMES = "idle_master_frames"
CONF_IDLE_SLAVE_FRAMES = "idle_slave_frames"
CONF_VMC_ONLINE_TIMEOUT = "vmc_online_timeout"

CONF_ON_MASTER_FRAME = "on_master_frame"
CONF_ON_SLAVE_FRAME = "on_slave_frame"
CONF_ON_VEND_COMPLETE = "on_vend_complete"
CONF_ON_CREDIT_CHANGE = "on_credit_change"
CONF_ON_VMC_STATUS = "on_vmc_status"

pulse_inspector_exe_ns = cg.esphome_ns.namespace("pulse_inspector_exe")
PulseInspectorExe = pulse_inspector_exe_ns.class_(
    "PulseInspectorExe", cg.Component
)
ParityEnum = pulse_inspector_exe_ns.enum("PulseInspectorExe::Parity", True)
PARITY_OPTIONS = {
    "none":  ParityEnum.NONE,
    "even":  ParityEnum.EVEN,
    "odd":   ParityEnum.ODD,
    "mark":  ParityEnum.MARK,
    "space": ParityEnum.SPACE,
}

# Trigger<std::vector<uint8_t> data, std::vector<uint8_t> mode, size_t len, bool suppressed>
_VECTOR_U8 = cg.std_vector.template(cg.uint8)
MasterFrameTrigger = pulse_inspector_exe_ns.class_(
    "MasterFrameTrigger",
    automation.Trigger.template(_VECTOR_U8, _VECTOR_U8, cg.size_t, cg.bool_),
)
SlaveFrameTrigger = pulse_inspector_exe_ns.class_(
    "SlaveFrameTrigger",
    automation.Trigger.template(_VECTOR_U8, _VECTOR_U8, cg.size_t, cg.bool_),
)

# Semantic-layer triggers. Argument tuples mirror the C++ Trigger
# templates in automation.h.
VendCompleteTrigger = pulse_inspector_exe_ns.class_(
    "VendCompleteTrigger",
    automation.Trigger.template(cg.bool_, cg.uint32, cg.uint8, cg.uint8),
)
CreditChangeTrigger = pulse_inspector_exe_ns.class_(
    "CreditChangeTrigger",
    automation.Trigger.template(cg.uint32, cg.int32, cg.uint16, cg.bool_),
)
VmcStatusTrigger = pulse_inspector_exe_ns.class_(
    "VmcStatusTrigger",
    automation.Trigger.template(cg.uint8, cg.bool_, cg.bool_, cg.uint8),
)


_HEX_BYTE_RE = re.compile(r"^[0-9A-Fa-f]{1,2}$")


def _hex_pattern(value):
    """Normalise an idle-frame pattern into a ``list[int]`` of bytes.

    Accepts any of the following input shapes (YAML quirks make all of
    them realistic):

    * ``"00 FE"`` / ``"00-FE"`` / ``"00:FE"`` / ``"00FE"`` -- one string
      with one or more hex bytes separated by whitespace, ``-``, ``:``,
      ``,`` or run together in pairs.
    * ``["00", "FE"]`` -- list of hex strings.
    * ``[0x00, 0xFE]`` -- list of pre-parsed integers (this is also how
      our own schema defaults arrive; ``0xFE`` becomes ``254`` which is
      no longer a valid two-character hex token, so we must NOT round-
      trip it through ``str()``).
    * Any mix of the two list forms.
    """

    def _coerce_one(item) -> int:
        # Already-decoded integer (YAML int, schema default, ...).
        if isinstance(item, bool):
            # Avoid Python's bool-is-int gotcha (True == 1, False == 0).
            raise cv.Invalid(f"Bool {item!r} is not a valid byte")
        if isinstance(item, int):
            if item < 0 or item > 0xFF:
                raise cv.Invalid(f"Byte {item} is out of range 0..255")
            return item
        # Hex string token.
        tok = str(item).strip()
        if not _HEX_BYTE_RE.match(tok):
            raise cv.Invalid(
                f"'{tok}' is not a valid hex byte. "
                "Patterns look like '00 FE' or '32 31'."
            )
        return int(tok, 16)

    items: list
    if isinstance(value, (bytes, bytearray)):
        items = list(value)
    elif isinstance(value, list):
        items = list(value)
    else:
        s = str(value).strip()
        s = s.replace("-", " ").replace(":", " ").replace(",", " ")
        if " " not in s and len(s) >= 2 and len(s) % 2 == 0:
            items = [s[i : i + 2] for i in range(0, len(s), 2)]
        else:
            items = s.split()

    if not items:
        raise cv.Invalid("Empty pattern (need at least one byte)")
    out = [_coerce_one(it) for it in items]
    if len(out) > 16:
        raise cv.Invalid(
            "Idle pattern is too long (max 16 bytes); if you need more, "
            "filter inside on_master_frame / on_slave_frame instead."
        )
    return out


def _trigger_schema(trigger_class):
    return automation.validate_automation(
        {
            cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(trigger_class),
        }
    )


# Default idle patterns derived from MEI Protocol A §4.4.5
# ("Polling of VMC, Credit in System"): the executive polls with 0x31
# (STATUS) and 0x32 (CREDIT), the VMC answers 0x00 (ACK) and 0xFE
# (No vend request). Each byte arrives as its own frame at the
# default 30-bit inter-byte timeout (~3 ms), so we keep them as
# 1-byte patterns. Users on other peripherals (ASU = 010xxxxx,
# CPP = 011xxxxx) can override or clear these lists.
_DEFAULT_IDLE_EXECUTIVE = [[0x31], [0x32]]
_DEFAULT_IDLE_PERIPHERAL = [[0x00], [0xFE]]


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PulseInspectorExe),
        cv.Required(CONF_INSPECTOR_ID): cv.use_id(PulseInspector),
        # Index of the parent's channel that taps the executive's TX
        # current loop -- the wire on which the executive sources
        # commands like 0x31 STATUS, 0x32 CREDIT. From the VMC's
        # point of view this is its RX wire.
        cv.Required(CONF_EXECUTIVE_CHANNEL): cv.int_range(min=0, max=15),
        # Index of the parent's channel that taps the peripheral's
        # reply current loop (RX from the executive's side).
        cv.Required(CONF_PERIPHERAL_CHANNEL): cv.int_range(min=0, max=15),
        cv.Optional(CONF_BAUD, default=9600): cv.int_range(min=300, max=1_000_000),
        # 8E1 per MEI Protocol A. Override only if you really know your
        # machine speaks something else on the EXE wire.
        cv.Optional(CONF_PARITY, default="even"): cv.enum(
            PARITY_OPTIONS, lower=True
        ),
        # 0 -> auto (computed in setup() as 30 * bit_us). Anything from
        # 200 us (highly aggressive grouping) up to 50 ms is allowed.
        cv.Optional(CONF_INTER_BYTE_TIMEOUT, default="0us"): cv.Any(
            cv.int_range(min=0, max=0),  # explicit "0" stays 0 == auto
            cv.positive_time_period_microseconds,
        ),
        cv.Optional(CONF_LOG_RAW_FRAMES, default=True): cv.boolean,
        cv.Optional(CONF_SUPPRESS_IDLE_POLLS, default=True): cv.boolean,
        cv.Optional(CONF_SHOW_PARITY_IN_HEX, default=False): cv.boolean,
        # 0 disables the watchdog (vmc_online stays at whatever the last
        # reply set it to). Default 5 s = ~50x the ~85 ms idle poll
        # cycle, which absorbs short stalls without false-OFFLINE.
        cv.Optional(CONF_VMC_ONLINE_TIMEOUT, default="5s"): cv.Any(
            cv.int_range(min=0, max=0),
            cv.positive_time_period_milliseconds,
        ),
        cv.Optional(
            CONF_IDLE_MASTER_FRAMES, default=_DEFAULT_IDLE_EXECUTIVE
        ): cv.ensure_list(_hex_pattern),
        cv.Optional(
            CONF_IDLE_SLAVE_FRAMES, default=_DEFAULT_IDLE_PERIPHERAL
        ): cv.ensure_list(_hex_pattern),
        cv.Optional(CONF_ON_MASTER_FRAME): _trigger_schema(MasterFrameTrigger),
        cv.Optional(CONF_ON_SLAVE_FRAME): _trigger_schema(SlaveFrameTrigger),
        cv.Optional(CONF_ON_VEND_COMPLETE): _trigger_schema(VendCompleteTrigger),
        cv.Optional(CONF_ON_CREDIT_CHANGE): _trigger_schema(CreditChangeTrigger),
        cv.Optional(CONF_ON_VMC_STATUS): _trigger_schema(VmcStatusTrigger),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_INSPECTOR_ID])
    cg.add(var.set_inspector(parent))

    cg.add(var.set_master_channel_index(config[CONF_EXECUTIVE_CHANNEL]))
    cg.add(var.set_slave_channel_index(config[CONF_PERIPHERAL_CHANNEL]))
    cg.add(var.set_baud(config[CONF_BAUD]))
    cg.add(var.set_parity(config[CONF_PARITY]))

    ibt = config[CONF_INTER_BYTE_TIMEOUT]
    # Both the legacy "0" int and the time-period object normalise to a
    # microsecond integer here.
    if hasattr(ibt, "total_microseconds"):
        ibt = int(ibt.total_microseconds)
    cg.add(var.set_inter_byte_timeout_us(int(ibt)))

    cg.add(var.set_log_raw_frames(config[CONF_LOG_RAW_FRAMES]))
    cg.add(var.set_suppress_idle_polls(config[CONF_SUPPRESS_IDLE_POLLS]))
    cg.add(var.set_show_parity_in_hex(config[CONF_SHOW_PARITY_IN_HEX]))

    online_to = config[CONF_VMC_ONLINE_TIMEOUT]
    if hasattr(online_to, "total_milliseconds"):
        online_to_ms = int(online_to.total_milliseconds)
    else:
        online_to_ms = int(online_to)
    cg.add(var.set_vmc_online_timeout_ms(online_to_ms))

    for pat in config[CONF_IDLE_MASTER_FRAMES]:
        cg.add(var.add_idle_master_pattern(list(pat)))
    for pat in config[CONF_IDLE_SLAVE_FRAMES]:
        cg.add(var.add_idle_slave_pattern(list(pat)))

    _FRAME_ARGS = [
        (_VECTOR_U8, "data"),
        (_VECTOR_U8, "mode"),
        (cg.size_t, "len"),
        (cg.bool_, "suppressed"),
    ]
    _VEND_ARGS = [
        (cg.bool_, "ok"),
        (cg.uint32, "value"),
        (cg.uint8, "selection_price"),
        (cg.uint8, "audit_pairs_pending"),
    ]
    _CREDIT_ARGS = [
        (cg.uint32, "credit"),
        (cg.int32, "delta"),
        (cg.uint16, "base_units"),
        (cg.bool_, "exact_change_only"),
    ]
    _STATUS_ARGS = [
        (cg.uint8, "status_raw"),
        (cg.bool_, "vending_inhibited"),
        (cg.bool_, "free_vend_request"),
        (cg.uint8, "audit_pairs_pending"),
    ]
    for key, args in (
        (CONF_ON_MASTER_FRAME, _FRAME_ARGS),
        (CONF_ON_SLAVE_FRAME, _FRAME_ARGS),
        (CONF_ON_VEND_COMPLETE, _VEND_ARGS),
        (CONF_ON_CREDIT_CHANGE, _CREDIT_ARGS),
        (CONF_ON_VMC_STATUS, _STATUS_ARGS),
    ):
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, args, conf)
