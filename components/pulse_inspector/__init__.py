"""ESPHome external component: pulse_inspector.

A generic GPIO->GPIO pulse inspector. Each channel mirrors the input pin level
to the output pin at the ISR level (fast, transparent pass-through) and
pushes every edge into a FreeRTOS queue for processing in a background task.
Multiple independent channels are supported, with future hooks for packet
decoding and traffic modification.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID

CODEOWNERS = ["@anat0l"]
DEPENDENCIES = ["esp32"]
MULTI_CONF = False

pulse_inspector_ns = cg.esphome_ns.namespace("pulse_inspector")
PulseInspector = pulse_inspector_ns.class_("PulseInspector", cg.Component)
PulseInspectorChannel = pulse_inspector_ns.class_("PulseInspectorChannel")

CONF_CHANNELS = "channels"
CONF_INPUT_PIN = "input_pin"
CONF_OUTPUT_PIN = "output_pin"
CONF_INVERT_IN = "invert_in"
CONF_INVERT_OUT = "invert_out"
CONF_QUEUE_SIZE = "queue_size"

CHANNEL_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PulseInspectorChannel),
        cv.Required(CONF_INPUT_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_OUTPUT_PIN): pins.internal_gpio_output_pin_schema,
        cv.Optional(CONF_INVERT_IN, default=False): cv.boolean,
        cv.Optional(CONF_INVERT_OUT, default=False): cv.boolean,
        cv.Optional(CONF_QUEUE_SIZE, default=256): cv.int_range(min=16, max=8192),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PulseInspector),
        cv.Required(CONF_CHANNELS): cv.ensure_list(
            cv.All(CHANNEL_SCHEMA, cv.has_at_least_one_key(CONF_INPUT_PIN))
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    parent = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(parent, config)

    for ch_conf in config[CONF_CHANNELS]:
        ch = cg.new_Pvariable(ch_conf[CONF_ID])

        input_pin = await cg.gpio_pin_expression(ch_conf[CONF_INPUT_PIN])
        cg.add(ch.set_input_pin(input_pin))

        if CONF_OUTPUT_PIN in ch_conf:
            output_pin = await cg.gpio_pin_expression(ch_conf[CONF_OUTPUT_PIN])
            cg.add(ch.set_output_pin(output_pin))

        cg.add(ch.set_invert_in(ch_conf[CONF_INVERT_IN]))
        cg.add(ch.set_invert_out(ch_conf[CONF_INVERT_OUT]))
        cg.add(ch.set_queue_size(ch_conf[CONF_QUEUE_SIZE]))

        cg.add(parent.add_channel(ch))
