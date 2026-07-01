"""Text sensors for pulse_inspector_exe.

Two groups of text sensors are exposed:

1. Raw frame mirrors (``last_master_frame`` / ``last_slave_frame``):
   the most recent non-suppressed master/slave frame as a hex dump
   with optional 9th-bit annotation (``00(0) FE(1)``). Idle-pattern
   frames do NOT update these so the display stays on the last
   "interesting" frame.

2. Semantic decode mirrors:

   - last_command       : human-readable name of the most recent
                          executive command byte ("31 VMC STATUS",
                          "38 VMC ACCEPT DATA", ...). Useful as a
                          live "what is the executive doing?"
                          indicator on a Home Assistant dashboard.
   - last_vend_outcome  : "OK" or "FAILED", set when a 0x33 VEND
                          reply arrives. Stays at the previous value
                          between vends.
   - last_status_text   : "0x10 INHIBITED STD pairs=2" -- decoded
                          STATUS reply with flag names and the
                          audit-pair counter, useful for at-a-glance
                          diagnostics.

All entries are optional.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import PulseInspectorExe

DEPENDENCIES = ["pulse_inspector_exe"]
CODEOWNERS = ["@anat0l"]

CONF_EXE_ID = "exe_id"
CONF_LAST_MASTER_FRAME = "last_master_frame"
CONF_LAST_SLAVE_FRAME = "last_slave_frame"
CONF_LAST_COMMAND = "last_command"
CONF_LAST_VEND_OUTCOME = "last_vend_outcome"
CONF_LAST_STATUS_TEXT = "last_status_text"


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EXE_ID): cv.use_id(PulseInspectorExe),
        cv.Optional(CONF_LAST_MASTER_FRAME): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_SLAVE_FRAME): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_COMMAND): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_VEND_OUTCOME): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_STATUS_TEXT): text_sensor.text_sensor_schema(),
    }
)

_BINDINGS = [
    (CONF_LAST_MASTER_FRAME, "set_last_master_frame_ts"),
    (CONF_LAST_SLAVE_FRAME, "set_last_slave_frame_ts"),
    (CONF_LAST_COMMAND, "set_last_command_ts"),
    (CONF_LAST_VEND_OUTCOME, "set_last_vend_outcome_ts"),
    (CONF_LAST_STATUS_TEXT, "set_last_status_text_ts"),
]


async def to_code(config):
    parent = await cg.get_variable(config[CONF_EXE_ID])
    for key, setter_name in _BINDINGS:
        if key in config:
            ts = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(parent, setter_name)(ts))
