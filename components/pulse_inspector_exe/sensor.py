"""Numeric sensors for pulse_inspector_exe.

Two groups of sensors are exposed:

1. Diagnostic counters (master_bytes, slave_bytes, frames, errors, ...):
   tick while the bus is alive. These are useful for verifying the tap
   and tuning idle-poll suppression.

2. Semantic application-layer sensors. Decoded directly from the MEI
   Protocol A traffic (see Executiv-MEI-000304001-Protocol-A-Y2.pdf):

   - current_credit              real-money credit on the VMC display
                                 (= base_units * scaling_factor),
                                 updated on every ACCEPT DATA block
   - current_credit_base_units   the raw 16-bit base-units field
                                 (handy for cross-checking machines
                                 that don't post-multiply by scaling)
   - current_selection_price     last 0x32 CREDIT poll reply when a
                                 selection is active (0xFE -> 0)
   - last_vend_value             real-money value of the last
                                 successful VEND
   - vends_ok_total              running count of Vend OK replies
   - vends_failed_total          running count of Vend Failed replies
   - money_inserted_total        running sum of positive credit deltas
                                 (coins/bills accepted by the machine)
   - money_change_total          running sum of negative credit deltas
                                 NOT attributable to a vend (change
                                 returned, manual cancels)
   - audit_pairs_pending         bits 4..0 of the last STATUS reply
                                 (how many audit pairs the VMC wants
                                 the executive to read)
   - last_audit_address          last audit-pair address seen on the
                                 wire (either VMC sending audit data,
                                 or executive writing to ASU)
   - last_audit_value            audit-pair data byte for the same
   - scaling_factor              last 8-bit scaling factor seen in an
                                 ACCEPT DATA block

All entries are optional; only the ones declared in YAML are wired up.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_EMPTY,
)

from . import PulseInspectorExe

DEPENDENCIES = ["pulse_inspector_exe"]
CODEOWNERS = ["@anat0l"]

CONF_EXE_ID = "exe_id"
CONF_FRAMES_DECODED = "frames_decoded"
CONF_FRAMING_ERRORS = "framing_errors"
CONF_PARITY_ERRORS = "parity_errors"
CONF_MASTER_BYTES = "master_bytes"
CONF_SLAVE_BYTES = "slave_bytes"
CONF_MASTER_FRAMES = "master_frames"
CONF_SLAVE_FRAMES = "slave_frames"
CONF_IDLE_POLLS = "idle_polls"
CONF_EVENTS_DROPPED = "events_dropped"

# Semantic sensors.
CONF_CURRENT_CREDIT = "current_credit"
CONF_CURRENT_CREDIT_BASE_UNITS = "current_credit_base_units"
CONF_CURRENT_SELECTION_PRICE = "current_selection_price"
CONF_LAST_VEND_VALUE = "last_vend_value"
CONF_VENDS_OK_TOTAL = "vends_ok_total"
CONF_VENDS_FAILED_TOTAL = "vends_failed_total"
CONF_MONEY_INSERTED_TOTAL = "money_inserted_total"
CONF_MONEY_CHANGE_TOTAL = "money_change_total"
CONF_AUDIT_PAIRS_PENDING = "audit_pairs_pending"
CONF_LAST_AUDIT_ADDRESS = "last_audit_address"
CONF_LAST_AUDIT_VALUE = "last_audit_value"
CONF_SCALING_FACTOR = "scaling_factor"


def _counter_schema():
    return sensor.sensor_schema(
        accuracy_decimals=0,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        unit_of_measurement=UNIT_EMPTY,
    )


def _gauge_schema():
    return sensor.sensor_schema(
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        unit_of_measurement=UNIT_EMPTY,
    )


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EXE_ID): cv.use_id(PulseInspectorExe),
        # Diagnostic counters.
        cv.Optional(CONF_FRAMES_DECODED): _counter_schema(),
        cv.Optional(CONF_FRAMING_ERRORS): _counter_schema(),
        cv.Optional(CONF_PARITY_ERRORS): _counter_schema(),
        cv.Optional(CONF_MASTER_BYTES): _counter_schema(),
        cv.Optional(CONF_SLAVE_BYTES): _counter_schema(),
        cv.Optional(CONF_MASTER_FRAMES): _counter_schema(),
        cv.Optional(CONF_SLAVE_FRAMES): _counter_schema(),
        cv.Optional(CONF_IDLE_POLLS): _counter_schema(),
        cv.Optional(CONF_EVENTS_DROPPED): _counter_schema(),
        # Semantic state (gauge-style: current value, not monotonic).
        cv.Optional(CONF_CURRENT_CREDIT): _gauge_schema(),
        cv.Optional(CONF_CURRENT_CREDIT_BASE_UNITS): _gauge_schema(),
        cv.Optional(CONF_CURRENT_SELECTION_PRICE): _gauge_schema(),
        cv.Optional(CONF_LAST_VEND_VALUE): _gauge_schema(),
        cv.Optional(CONF_AUDIT_PAIRS_PENDING): _gauge_schema(),
        cv.Optional(CONF_LAST_AUDIT_ADDRESS): _gauge_schema(),
        cv.Optional(CONF_LAST_AUDIT_VALUE): _gauge_schema(),
        cv.Optional(CONF_SCALING_FACTOR): _gauge_schema(),
        # Semantic running totals.
        cv.Optional(CONF_VENDS_OK_TOTAL): _counter_schema(),
        cv.Optional(CONF_VENDS_FAILED_TOTAL): _counter_schema(),
        cv.Optional(CONF_MONEY_INSERTED_TOTAL): _counter_schema(),
        cv.Optional(CONF_MONEY_CHANGE_TOTAL): _counter_schema(),
    }
)

_SENSOR_BINDINGS = [
    (CONF_FRAMES_DECODED, "set_frames_decoded_sensor"),
    (CONF_FRAMING_ERRORS, "set_framing_errors_sensor"),
    (CONF_PARITY_ERRORS, "set_parity_errors_sensor"),
    (CONF_MASTER_BYTES, "set_master_bytes_sensor"),
    (CONF_SLAVE_BYTES, "set_slave_bytes_sensor"),
    (CONF_MASTER_FRAMES, "set_master_frames_sensor"),
    (CONF_SLAVE_FRAMES, "set_slave_frames_sensor"),
    (CONF_IDLE_POLLS, "set_idle_polls_sensor"),
    (CONF_EVENTS_DROPPED, "set_events_dropped_sensor"),
    (CONF_CURRENT_CREDIT, "set_current_credit_sensor"),
    (CONF_CURRENT_CREDIT_BASE_UNITS, "set_current_credit_base_units_sensor"),
    (CONF_CURRENT_SELECTION_PRICE, "set_current_selection_price_sensor"),
    (CONF_LAST_VEND_VALUE, "set_last_vend_value_sensor"),
    (CONF_VENDS_OK_TOTAL, "set_vends_ok_total_sensor"),
    (CONF_VENDS_FAILED_TOTAL, "set_vends_failed_total_sensor"),
    (CONF_MONEY_INSERTED_TOTAL, "set_money_inserted_total_sensor"),
    (CONF_MONEY_CHANGE_TOTAL, "set_money_change_total_sensor"),
    (CONF_AUDIT_PAIRS_PENDING, "set_audit_pairs_pending_sensor"),
    (CONF_LAST_AUDIT_ADDRESS, "set_last_audit_address_sensor"),
    (CONF_LAST_AUDIT_VALUE, "set_last_audit_value_sensor"),
    (CONF_SCALING_FACTOR, "set_scaling_factor_sensor"),
]


async def to_code(config):
    parent = await cg.get_variable(config[CONF_EXE_ID])
    for key, setter_name in _SENSOR_BINDINGS:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(parent, setter_name)(sens))
