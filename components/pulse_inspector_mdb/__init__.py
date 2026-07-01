"""MDB (Multi-Drop Bus) decoder for PulseInspector.

Subscribes to one or two channels of a parent ``pulse_inspector`` and
decodes the edges as 9-bit UART (NAMA MDB default: 9600 baud, 1 start +
9 data + 1 stop, LSB first, no parity). Each decoded 9-bit word is
interpreted semantically against the NAMA MDB 4.x command set.

Two operating modes are supported:

1. Single-wire mode (``channel: N``). Both directions share the same
   physical wire; the 9th data bit distinguishes master address/command
   bytes (bit9=1) from peripheral data bytes (bit9=0).

2. Two-wire mode (``tx_channel: N, rx_channel: M``). Each physical MDB
   wire is tapped on its own GPIO. The TX line (from the VMC, master)
   and the RX line (from peripherals, slave) are decoded independently,
   so multi-byte VMC commands (address byte + data bytes with bit9=0)
   are correctly attributed to the master.

The decoder produces semantic events that surface as:

  * INFO-level log lines with human-readable descriptions.
  * Triggers for YAML automations (``on_vend_request``, ``on_vend_success``,
    ``on_vend_failure``, ``on_cash_sale``, ``on_session_begin``,
    ``on_session_end``, ``on_bv_error``, ``on_changer_error``,
    ``on_cashless_error``, ``on_bill_accepted``, ``on_coin_deposited``,
    ``on_sale_cycle_begin``, ``on_sale_cycle_end``, ``on_event``).
  * Sensors, binary_sensors, and text_sensors through separate platforms.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_CHANNEL, CONF_TRIGGER_ID, CONF_NAME

from esphome.components.pulse_inspector import (
    pulse_inspector_ns,  # noqa: F401
    PulseInspector,
)

CODEOWNERS = ["@anat0l"]
DEPENDENCIES = ["esp32", "pulse_inspector"]
MULTI_CONF = True

CONF_INSPECTOR_ID = "inspector_id"
CONF_TX_CHANNEL = "tx_channel"
CONF_RX_CHANNEL = "rx_channel"
CONF_BAUD = "baud"
CONF_LOG_SLAVE_ACK = "log_slave_ack"
CONF_LOG_RAW_FRAMES = "log_raw_frames"
CONF_SUPPRESS_IDLE_POLLS = "suppress_idle_polls"
CONF_SELECTION_MAP = "selection_map"
CONF_ITEM = "item"

CONF_ON_EVENT = "on_event"
CONF_ON_VEND_REQUEST = "on_vend_request"
CONF_ON_VEND_APPROVED = "on_vend_approved"
CONF_ON_VEND_DENIED = "on_vend_denied"
CONF_ON_VEND_SUCCESS = "on_vend_success"
CONF_ON_VEND_FAILURE = "on_vend_failure"
CONF_ON_CASH_SALE = "on_cash_sale"
CONF_ON_SESSION_BEGIN = "on_session_begin"
CONF_ON_SESSION_END = "on_session_end"
CONF_ON_BV_ERROR = "on_bv_error"
CONF_ON_BILL_ACCEPTED = "on_bill_accepted"
CONF_ON_CHANGER_ERROR = "on_changer_error"
CONF_ON_COIN_DEPOSITED = "on_coin_deposited"
CONF_ON_CASHLESS_ERROR = "on_cashless_error"
CONF_ON_SALE_CYCLE_BEGIN = "on_sale_cycle_begin"
CONF_ON_SALE_CYCLE_END = "on_sale_cycle_end"

pulse_inspector_mdb_ns = cg.esphome_ns.namespace("pulse_inspector_mdb")
PulseInspectorMdb = pulse_inspector_mdb_ns.class_(
    "PulseInspectorMdb", cg.Component
)

EventTrigger = pulse_inspector_mdb_ns.class_(
    "EventTrigger", automation.Trigger.template(cg.std_string)
)
VendRequestTrigger = pulse_inspector_mdb_ns.class_(
    "VendRequestTrigger", automation.Trigger.template(cg.uint32, cg.uint32)
)
VendApprovedTrigger = pulse_inspector_mdb_ns.class_(
    "VendApprovedTrigger", automation.Trigger.template(cg.uint32)
)
VendDeniedTrigger = pulse_inspector_mdb_ns.class_(
    "VendDeniedTrigger", automation.Trigger.template()
)
VendSuccessTrigger = pulse_inspector_mdb_ns.class_(
    "VendSuccessTrigger", automation.Trigger.template(cg.uint32)
)
VendFailureTrigger = pulse_inspector_mdb_ns.class_(
    "VendFailureTrigger", automation.Trigger.template(cg.uint32)
)
CashSaleTrigger = pulse_inspector_mdb_ns.class_(
    "CashSaleTrigger", automation.Trigger.template(cg.uint32, cg.uint32)
)
SessionBeginTrigger = pulse_inspector_mdb_ns.class_(
    "SessionBeginTrigger", automation.Trigger.template(cg.uint32)
)
SessionEndTrigger = pulse_inspector_mdb_ns.class_(
    "SessionEndTrigger", automation.Trigger.template()
)
BvErrorTrigger = pulse_inspector_mdb_ns.class_(
    "BvErrorTrigger", automation.Trigger.template(cg.uint8, cg.std_string)
)
BillAcceptedTrigger = pulse_inspector_mdb_ns.class_(
    "BillAcceptedTrigger", automation.Trigger.template(cg.uint8, cg.bool_)
)
ChangerErrorTrigger = pulse_inspector_mdb_ns.class_(
    "ChangerErrorTrigger", automation.Trigger.template(cg.uint8, cg.std_string)
)
CoinDepositedTrigger = pulse_inspector_mdb_ns.class_(
    "CoinDepositedTrigger", automation.Trigger.template(cg.uint8, cg.uint8)
)
CashlessErrorTrigger = pulse_inspector_mdb_ns.class_(
    "CashlessErrorTrigger", automation.Trigger.template(cg.uint8, cg.std_string)
)
SaleCycleBeginTrigger = pulse_inspector_mdb_ns.class_(
    "SaleCycleBeginTrigger", automation.Trigger.template()
)
SaleCycleEndTrigger = pulse_inspector_mdb_ns.class_(
    "SaleCycleEndTrigger", automation.Trigger.template(cg.uint32)
)


def _validate_channel_config(config):
    has_single = CONF_CHANNEL in config
    has_tx = CONF_TX_CHANNEL in config
    has_rx = CONF_RX_CHANNEL in config

    if has_single and (has_tx or has_rx):
        raise cv.Invalid(
            f"Use either '{CONF_CHANNEL}' (single-wire mode) "
            f"or both '{CONF_TX_CHANNEL}' and '{CONF_RX_CHANNEL}' "
            f"(two-wire mode), not a mix."
        )
    if has_tx != has_rx:
        raise cv.Invalid(
            f"Two-wire mode requires BOTH '{CONF_TX_CHANNEL}' "
            f"and '{CONF_RX_CHANNEL}' to be set."
        )
    if not has_single and not has_tx:
        raise cv.Invalid(
            f"Must set either '{CONF_CHANNEL}' (single-wire) "
            f"or '{CONF_TX_CHANNEL}' + '{CONF_RX_CHANNEL}' (two-wire)."
        )
    if has_tx and config[CONF_TX_CHANNEL] == config[CONF_RX_CHANNEL]:
        raise cv.Invalid(
            f"'{CONF_TX_CHANNEL}' and '{CONF_RX_CHANNEL}' must differ."
        )
    return config


SELECTION_ENTRY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ITEM): cv.positive_int,
        cv.Required(CONF_NAME): cv.string_strict,
    }
)


def _trigger_schema(trigger_class):
    return automation.validate_automation(
        {
            cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(trigger_class),
        }
    )


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PulseInspectorMdb),
            cv.Required(CONF_INSPECTOR_ID): cv.use_id(PulseInspector),
            cv.Optional(CONF_CHANNEL): cv.int_range(min=0, max=15),
            cv.Optional(CONF_TX_CHANNEL): cv.int_range(min=0, max=15),
            cv.Optional(CONF_RX_CHANNEL): cv.int_range(min=0, max=15),
            cv.Optional(CONF_BAUD, default=9600): cv.int_range(
                min=300, max=1_000_000
            ),
            cv.Optional(CONF_LOG_SLAVE_ACK, default=False): cv.boolean,
            cv.Optional(CONF_LOG_RAW_FRAMES, default=False): cv.boolean,
            # When true, hides the raw hex dump of "idle" POLL / discovery
            # RESET commands (e.g. `master [2] 33 33`, `master [2] 10 10`) and
            # the single-byte ACK replies that immediately follow them. Real
            # events (VEND, SETUP, BV_STATUS, NAK/PNAK, multi-byte replies,
            # anything anomalous like `master [1] 33`) are still logged.
            cv.Optional(CONF_SUPPRESS_IDLE_POLLS, default=False): cv.boolean,
            # Map item_number -> human readable drink name, used by the
            # last_selection_name text sensor and in log lines.
            cv.Optional(CONF_SELECTION_MAP): cv.ensure_list(
                SELECTION_ENTRY_SCHEMA
            ),
            # Triggers -------------------------------------------------
            cv.Optional(CONF_ON_EVENT): _trigger_schema(EventTrigger),
            cv.Optional(CONF_ON_VEND_REQUEST): _trigger_schema(VendRequestTrigger),
            cv.Optional(CONF_ON_VEND_APPROVED): _trigger_schema(VendApprovedTrigger),
            cv.Optional(CONF_ON_VEND_DENIED): _trigger_schema(VendDeniedTrigger),
            cv.Optional(CONF_ON_VEND_SUCCESS): _trigger_schema(VendSuccessTrigger),
            cv.Optional(CONF_ON_VEND_FAILURE): _trigger_schema(VendFailureTrigger),
            cv.Optional(CONF_ON_CASH_SALE): _trigger_schema(CashSaleTrigger),
            cv.Optional(CONF_ON_SESSION_BEGIN): _trigger_schema(SessionBeginTrigger),
            cv.Optional(CONF_ON_SESSION_END): _trigger_schema(SessionEndTrigger),
            cv.Optional(CONF_ON_BV_ERROR): _trigger_schema(BvErrorTrigger),
            cv.Optional(CONF_ON_BILL_ACCEPTED): _trigger_schema(BillAcceptedTrigger),
            cv.Optional(CONF_ON_CHANGER_ERROR): _trigger_schema(ChangerErrorTrigger),
            cv.Optional(CONF_ON_COIN_DEPOSITED): _trigger_schema(CoinDepositedTrigger),
            cv.Optional(CONF_ON_CASHLESS_ERROR): _trigger_schema(CashlessErrorTrigger),
            cv.Optional(CONF_ON_SALE_CYCLE_BEGIN): _trigger_schema(
                SaleCycleBeginTrigger
            ),
            cv.Optional(CONF_ON_SALE_CYCLE_END): _trigger_schema(
                SaleCycleEndTrigger
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_channel_config,
)


# List of (config key, trigger class, automation args). Each arg tuple is
# (C++ type, lambda name) matching the trigger's Trigger<Ts...> template.
_TRIGGER_SPECS = [
    (CONF_ON_EVENT, EventTrigger, [(cg.std_string, "description")]),
    (
        CONF_ON_VEND_REQUEST,
        VendRequestTrigger,
        [(cg.uint32, "price_cents"), (cg.uint32, "item")],
    ),
    (CONF_ON_VEND_APPROVED, VendApprovedTrigger, [(cg.uint32, "price_cents")]),
    (CONF_ON_VEND_DENIED, VendDeniedTrigger, []),
    (CONF_ON_VEND_SUCCESS, VendSuccessTrigger, [(cg.uint32, "item")]),
    (CONF_ON_VEND_FAILURE, VendFailureTrigger, [(cg.uint32, "item")]),
    (
        CONF_ON_CASH_SALE,
        CashSaleTrigger,
        [(cg.uint32, "price_cents"), (cg.uint32, "item")],
    ),
    (CONF_ON_SESSION_BEGIN, SessionBeginTrigger, [(cg.uint32, "funds_cents")]),
    (CONF_ON_SESSION_END, SessionEndTrigger, []),
    (
        CONF_ON_BV_ERROR,
        BvErrorTrigger,
        [(cg.uint8, "code"), (cg.std_string, "description")],
    ),
    (
        CONF_ON_BILL_ACCEPTED,
        BillAcceptedTrigger,
        [(cg.uint8, "bill_type"), (cg.bool_, "to_stacker")],
    ),
    (
        CONF_ON_CHANGER_ERROR,
        ChangerErrorTrigger,
        [(cg.uint8, "code"), (cg.std_string, "description")],
    ),
    (
        CONF_ON_COIN_DEPOSITED,
        CoinDepositedTrigger,
        [(cg.uint8, "coin_type"), (cg.uint8, "count_in_tube")],
    ),
    (
        CONF_ON_CASHLESS_ERROR,
        CashlessErrorTrigger,
        [(cg.uint8, "code"), (cg.std_string, "description")],
    ),
    (CONF_ON_SALE_CYCLE_BEGIN, SaleCycleBeginTrigger, []),
    (CONF_ON_SALE_CYCLE_END, SaleCycleEndTrigger, [(cg.uint32, "duration_ms")]),
]


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_INSPECTOR_ID])
    cg.add(var.set_inspector(parent))

    if CONF_CHANNEL in config:
        cg.add(var.set_single_channel_index(config[CONF_CHANNEL]))
    else:
        cg.add(var.set_master_channel_index(config[CONF_TX_CHANNEL]))
        cg.add(var.set_slave_channel_index(config[CONF_RX_CHANNEL]))

    cg.add(var.set_baud(config[CONF_BAUD]))
    cg.add(var.set_log_slave_ack(config[CONF_LOG_SLAVE_ACK]))
    cg.add(var.set_log_raw_frames(config[CONF_LOG_RAW_FRAMES]))
    cg.add(var.set_suppress_idle_polls(config[CONF_SUPPRESS_IDLE_POLLS]))

    for entry in config.get(CONF_SELECTION_MAP, []):
        cg.add(var.add_selection_name(entry[CONF_ITEM], entry[CONF_NAME]))

    for key, _cls, args in _TRIGGER_SPECS:
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, args, conf)
