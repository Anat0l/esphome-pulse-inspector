"""Text sensors for pulse_inspector_mdb.

Expose human-readable descriptions of the most recent MDB events, last
fault on each peripheral, and last drink selection name.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import PulseInspectorMdb

DEPENDENCIES = ["pulse_inspector_mdb"]
CODEOWNERS = ["@anat0l"]

CONF_MDB_ID = "mdb_id"
CONF_LAST_EVENT = "last_event"
CONF_LAST_SELECTION_NAME = "last_selection_name"
CONF_LAST_BV_ERROR = "last_bv_error"
CONF_LAST_CHANGER_ERROR = "last_changer_error"
CONF_LAST_CASHLESS_ERROR = "last_cashless_error"
CONF_BV_ENABLE_MASK = "bv_enable_mask"


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_MDB_ID): cv.use_id(PulseInspectorMdb),
        cv.Optional(CONF_LAST_EVENT): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_SELECTION_NAME): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_BV_ERROR): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_CHANGER_ERROR): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_CASHLESS_ERROR): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_BV_ENABLE_MASK): text_sensor.text_sensor_schema(),
    }
)


_BINDINGS = [
    (CONF_LAST_EVENT, "set_last_event_ts"),
    (CONF_LAST_SELECTION_NAME, "set_last_selection_name_ts"),
    (CONF_LAST_BV_ERROR, "set_last_bv_error_ts"),
    (CONF_LAST_CHANGER_ERROR, "set_last_changer_error_ts"),
    (CONF_LAST_CASHLESS_ERROR, "set_last_cashless_error_ts"),
    (CONF_BV_ENABLE_MASK, "set_bv_enable_mask_ts"),
]


async def to_code(config):
    parent = await cg.get_variable(config[CONF_MDB_ID])
    for key, setter_name in _BINDINGS:
        if key in config:
            ts = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(parent, setter_name)(ts))
