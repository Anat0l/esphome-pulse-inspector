"""Example child component for pulse_inspector.

Subscribes to the pulse stream of a single PulseInspectorChannel and
periodically logs the number of edges observed since the previous report.
Use this as a copy-paste template for real decoders or injection modules.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

from esphome.components.pulse_inspector import PulseInspectorChannel

CODEOWNERS = ["@anat0l"]
DEPENDENCIES = ["esp32", "pulse_inspector"]
MULTI_CONF = True

CONF_CHANNEL_ID = "channel_id"
CONF_REPORT_INTERVAL = "report_interval"

pulse_inspector_logger_ns = cg.esphome_ns.namespace("pulse_inspector_logger")
PulseInspectorLogger = pulse_inspector_logger_ns.class_(
    "PulseInspectorLogger", cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PulseInspectorLogger),
        cv.Required(CONF_CHANNEL_ID): cv.use_id(PulseInspectorChannel),
        cv.Optional(
            CONF_REPORT_INTERVAL, default="5s"
        ): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    channel = await cg.get_variable(config[CONF_CHANNEL_ID])
    cg.add(var.set_channel(channel))
    cg.add(var.set_report_interval(config[CONF_REPORT_INTERVAL]))
