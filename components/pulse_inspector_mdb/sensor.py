"""Numeric sensors for pulse_inspector_mdb.

Every field is optional. Listed sensors are registered with the parent
PulseInspectorMdb and republished as MDB events are decoded.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_MONETARY,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_EMPTY,
    UNIT_SECOND,
)

from . import PulseInspectorMdb, pulse_inspector_mdb_ns  # noqa: F401

DEPENDENCIES = ["pulse_inspector_mdb"]
CODEOWNERS = ["@anat0l"]

CONF_MDB_ID = "mdb_id"
# --- Cashless sensors (only fire with a cashless reader on the bus) -------
CONF_LAST_ITEM = "last_item"
CONF_LAST_PRICE = "last_price"
CONF_SESSION_FUNDS = "session_funds"
CONF_VEND_SUCCESS_COUNT = "vend_success_count"
CONF_VEND_FAILURE_COUNT = "vend_failure_count"
# --- Always available (UART-level) ---------------------------------------
CONF_FRAMES_DECODED = "frames_decoded"
CONF_FRAMING_ERRORS = "framing_errors"
CONF_MASTER_BYTES = "master_bytes"
CONF_SLAVE_BYTES = "slave_bytes"
CONF_EVENTS_DROPPED = "events_dropped"
# Manufacturer-proprietary master frames that were hidden from the log
# (non-NAMA base addresses like 0xC0/0xF0/0xF8 and orphan mode=1 byte
# sequences). Harmless chatter between MEI peripherals -- exposed so the
# rate is visible without flooding "MDB: Last event".
CONF_PROPRIETARY_FRAMES = "proprietary_frames"
# --- BV-centric sensors (work on a coin-mech <-> BV tap) -----------------
CONF_BILLS_STACKED_COUNT = "bills_stacked_count"
CONF_BILLS_ESCROWED_COUNT = "bills_escrowed_count"
CONF_BILLS_RETURNED_COUNT = "bills_returned_count"
CONF_BILLS_REJECTED_COUNT = "bills_rejected_count"
CONF_LAST_BILL_TYPE = "last_bill_type"
# --- Derived sale-cycle sensors ------------------------------------------
CONF_SALE_CYCLES_TOTAL = "sale_cycles_total"
CONF_LAST_SALE_DURATION = "last_sale_duration"

UNIT_EURO = "\u20ac"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_MDB_ID): cv.use_id(PulseInspectorMdb),
        cv.Optional(CONF_LAST_ITEM): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_LAST_PRICE): sensor.sensor_schema(
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            unit_of_measurement=UNIT_EURO,
            device_class=DEVICE_CLASS_MONETARY,
        ),
        cv.Optional(CONF_SESSION_FUNDS): sensor.sensor_schema(
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            unit_of_measurement=UNIT_EURO,
            device_class=DEVICE_CLASS_MONETARY,
        ),
        cv.Optional(CONF_VEND_SUCCESS_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_VEND_FAILURE_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_FRAMES_DECODED): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_FRAMING_ERRORS): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_MASTER_BYTES): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_SLAVE_BYTES): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_EVENTS_DROPPED): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_PROPRIETARY_FRAMES): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_BILLS_STACKED_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_BILLS_ESCROWED_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_BILLS_RETURNED_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_BILLS_REJECTED_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_LAST_BILL_TYPE): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_SALE_CYCLES_TOTAL): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            unit_of_measurement=UNIT_EMPTY,
        ),
        cv.Optional(CONF_LAST_SALE_DURATION): sensor.sensor_schema(
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            unit_of_measurement=UNIT_SECOND,
            device_class=DEVICE_CLASS_DURATION,
        ),
    }
)


_SENSOR_BINDINGS = [
    (CONF_LAST_ITEM, "set_last_item_sensor"),
    (CONF_LAST_PRICE, "set_last_price_sensor"),
    (CONF_SESSION_FUNDS, "set_session_funds_sensor"),
    (CONF_VEND_SUCCESS_COUNT, "set_vend_success_count_sensor"),
    (CONF_VEND_FAILURE_COUNT, "set_vend_failure_count_sensor"),
    (CONF_FRAMES_DECODED, "set_frames_decoded_sensor"),
    (CONF_FRAMING_ERRORS, "set_framing_errors_sensor"),
    (CONF_MASTER_BYTES, "set_master_bytes_sensor"),
    (CONF_SLAVE_BYTES, "set_slave_bytes_sensor"),
    (CONF_EVENTS_DROPPED, "set_events_dropped_sensor"),
    (CONF_PROPRIETARY_FRAMES, "set_proprietary_frames_sensor"),
    (CONF_BILLS_STACKED_COUNT, "set_bills_stacked_sensor"),
    (CONF_BILLS_ESCROWED_COUNT, "set_bills_escrowed_sensor"),
    (CONF_BILLS_RETURNED_COUNT, "set_bills_returned_sensor"),
    (CONF_BILLS_REJECTED_COUNT, "set_bills_rejected_sensor"),
    (CONF_LAST_BILL_TYPE, "set_last_bill_type_sensor"),
    (CONF_SALE_CYCLES_TOTAL, "set_sale_cycles_total_sensor"),
    (CONF_LAST_SALE_DURATION, "set_last_sale_duration_sensor"),
]


async def to_code(config):
    parent = await cg.get_variable(config[CONF_MDB_ID])
    for key, setter_name in _SENSOR_BINDINGS:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(parent, setter_name)(sens))
