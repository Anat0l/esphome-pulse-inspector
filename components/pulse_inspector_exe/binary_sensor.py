"""Binary sensors for pulse_inspector_exe.

These map the boolean fields of the most recent VMC STATUS reply (and
the in-flight VEND state) onto Home Assistant binary sensors:

- vmc_online           : true while the VMC is actually answering
                         executive polls. Falls to false after
                         ``vmc_online_timeout`` (5 s by default) of
                         silence; this catches a wire disconnect or
                         a VMC reset cleanly.

- vending_inhibited    : bit 6 of the STATUS reply. Set by the VMC
                         when it has refused to vend (sold-out
                         everywhere, fault, service mode, ...). The
                         executive normally stops accepting money
                         while this is true.

- free_vend_request    : bit 7 of the STATUS reply. Set when a
                         currently selected drink has price 0
                         (test/promo button or a test selection from
                         the service panel).

- vend_in_progress     : true between the executive sending 0x33 VEND
                         and the VMC's reply (Vend OK / Vend Failed).
                         For a normal coffee cycle this is on for a
                         few hundred ms up to ~2 s.

All entries are optional.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import PulseInspectorExe

DEPENDENCIES = ["pulse_inspector_exe"]
CODEOWNERS = ["@anat0l"]

CONF_EXE_ID = "exe_id"
CONF_VMC_ONLINE = "vmc_online"
CONF_VENDING_INHIBITED = "vending_inhibited"
CONF_FREE_VEND_REQUEST = "free_vend_request"
CONF_VEND_IN_PROGRESS = "vend_in_progress"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EXE_ID): cv.use_id(PulseInspectorExe),
        cv.Optional(CONF_VMC_ONLINE): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_VENDING_INHIBITED): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_FREE_VEND_REQUEST): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_VEND_IN_PROGRESS): binary_sensor.binary_sensor_schema(),
    }
)

_BINDINGS = [
    (CONF_VMC_ONLINE, "set_vmc_online_bs"),
    (CONF_VENDING_INHIBITED, "set_vending_inhibited_bs"),
    (CONF_FREE_VEND_REQUEST, "set_free_vend_request_bs"),
    (CONF_VEND_IN_PROGRESS, "set_vend_in_progress_bs"),
]


async def to_code(config):
    parent = await cg.get_variable(config[CONF_EXE_ID])
    for key, setter_name in _BINDINGS:
        if key in config:
            bs = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(parent, setter_name)(bs))
