"""Binary sensors for pulse_inspector_mdb.

Expose latched / stateful boolean flags derived from the MDB event stream:
jam flags on the BV/changer, cashless malfunction, session/vend activity.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_CONNECTIVITY,
)

from . import PulseInspectorMdb

DEPENDENCIES = ["pulse_inspector_mdb"]
CODEOWNERS = ["@anat0l"]

CONF_MDB_ID = "mdb_id"
CONF_BV_JAM = "bv_jam"
CONF_BV_DISABLED = "bv_disabled"
CONF_CHANGER_JAM = "changer_jam"
CONF_CASHLESS_MALFUNCTION = "cashless_malfunction"
CONF_SESSION_ACTIVE = "session_active"
CONF_VEND_IN_PROGRESS = "vend_in_progress"
CONF_SALE_CYCLE_IN_PROGRESS = "sale_cycle_in_progress"


def _bs_schema(device_class: str):
    return binary_sensor.binary_sensor_schema(device_class=device_class)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_MDB_ID): cv.use_id(PulseInspectorMdb),
        cv.Optional(CONF_BV_JAM): _bs_schema(DEVICE_CLASS_PROBLEM),
        cv.Optional(CONF_BV_DISABLED): _bs_schema(DEVICE_CLASS_CONNECTIVITY),
        cv.Optional(CONF_CHANGER_JAM): _bs_schema(DEVICE_CLASS_PROBLEM),
        cv.Optional(CONF_CASHLESS_MALFUNCTION): _bs_schema(DEVICE_CLASS_PROBLEM),
        cv.Optional(CONF_SESSION_ACTIVE): _bs_schema(DEVICE_CLASS_RUNNING),
        cv.Optional(CONF_VEND_IN_PROGRESS): _bs_schema(DEVICE_CLASS_RUNNING),
        cv.Optional(CONF_SALE_CYCLE_IN_PROGRESS): _bs_schema(DEVICE_CLASS_RUNNING),
    }
)


_BINDINGS = [
    (CONF_BV_JAM, "set_bv_jam_bs"),
    (CONF_BV_DISABLED, "set_bv_disabled_bs"),
    (CONF_CHANGER_JAM, "set_changer_jam_bs"),
    (CONF_CASHLESS_MALFUNCTION, "set_cashless_malfunction_bs"),
    (CONF_SESSION_ACTIVE, "set_session_active_bs"),
    (CONF_VEND_IN_PROGRESS, "set_vend_in_progress_bs"),
    (CONF_SALE_CYCLE_IN_PROGRESS, "set_sale_cycle_in_progress_bs"),
]


async def to_code(config):
    parent = await cg.get_variable(config[CONF_MDB_ID])
    for key, setter_name in _BINDINGS:
        if key in config:
            bs = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(parent, setter_name)(bs))
