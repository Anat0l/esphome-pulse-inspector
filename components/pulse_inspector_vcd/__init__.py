"""PulseInspector VCD exporter.

Subscribes to every channel of a parent ``pulse_inspector`` and dumps the
captured edges on demand as a Value Change Dump file over a raw TCP port.
Any VCD viewer (PulseView, GTKWave, ...) can then open the capture as a
multi-channel oscillogram with protocol decoders.

Download a snapshot with any TCP client, e.g.:

    nc <device-ip> 9000 > capture.vcd
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT

from esphome.components.pulse_inspector import (
    pulse_inspector_ns,  # noqa: F401 — imported for codegen side effects
    PulseInspector,
)

CODEOWNERS = ["@anat0l"]
DEPENDENCIES = ["esp32", "pulse_inspector"]
MULTI_CONF = False

CONF_INSPECTOR_ID = "inspector_id"
CONF_BUFFER_SIZE = "buffer_size"

# Per-connection knobs (see pulse_inspector_vcd.h for semantics).
CONF_SEND_TIMEOUT = "send_timeout"
CONF_LIVE_QUEUE_DEPTH = "live_queue_depth"
CONF_APP_KEEPALIVE_INTERVAL = "app_keepalive_interval"
CONF_TCP_KEEPALIVE = "tcp_keepalive"
CONF_TCP_KEEPALIVE_ENABLED = "enabled"
CONF_TCP_KEEPALIVE_IDLE = "idle"
CONF_TCP_KEEPALIVE_INTERVAL = "interval"
CONF_TCP_KEEPALIVE_COUNT = "count"

pulse_inspector_vcd_ns = cg.esphome_ns.namespace("pulse_inspector_vcd")
PulseInspectorVcd = pulse_inspector_vcd_ns.class_(
    "PulseInspectorVcd", cg.Component
)


def _bounded_seconds(min_s, max_s, name):
    """Validator that accepts an ESPHome time-period and bounds it in seconds."""

    def _v(value):
        period = cv.positive_time_period_seconds(value)
        if period.total_seconds < min_s:
            raise cv.Invalid(f"{name} must be >= {min_s}s")
        if period.total_seconds > max_s:
            raise cv.Invalid(f"{name} must be <= {max_s}s")
        return period

    return _v


TCP_KEEPALIVE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TCP_KEEPALIVE_ENABLED, default=True): cv.boolean,
        cv.Optional(CONF_TCP_KEEPALIVE_IDLE, default="30s"): _bounded_seconds(
            1, 3600, "tcp_keepalive.idle"
        ),
        cv.Optional(
            CONF_TCP_KEEPALIVE_INTERVAL, default="10s"
        ): _bounded_seconds(1, 600, "tcp_keepalive.interval"),
        cv.Optional(CONF_TCP_KEEPALIVE_COUNT, default=3): cv.int_range(
            min=1, max=20
        ),
    }
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PulseInspectorVcd),
        cv.Required(CONF_INSPECTOR_ID): cv.use_id(PulseInspector),
        cv.Optional(CONF_BUFFER_SIZE, default=4096): cv.int_range(
            min=128, max=65536
        ),
        cv.Optional(CONF_PORT, default=9000): cv.port,
        cv.Optional(CONF_SEND_TIMEOUT, default="30s"): _bounded_seconds(
            1, 3600, "send_timeout"
        ),
        cv.Optional(CONF_LIVE_QUEUE_DEPTH, default=1024): cv.int_range(
            min=64, max=16384
        ),
        # 0s disables the application-layer $comment heartbeat.
        cv.Optional(
            CONF_APP_KEEPALIVE_INTERVAL, default="15s"
        ): _bounded_seconds(0, 3600, "app_keepalive_interval"),
        cv.Optional(CONF_TCP_KEEPALIVE, default={}): TCP_KEEPALIVE_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_INSPECTOR_ID])
    cg.add(var.set_inspector(parent))
    cg.add(var.set_buffer_size(config[CONF_BUFFER_SIZE]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_send_timeout_sec(int(config[CONF_SEND_TIMEOUT].total_seconds)))
    cg.add(var.set_live_queue_depth(config[CONF_LIVE_QUEUE_DEPTH]))
    cg.add(
        var.set_app_keepalive_interval_sec(
            int(config[CONF_APP_KEEPALIVE_INTERVAL].total_seconds)
        )
    )
    ka = config[CONF_TCP_KEEPALIVE]
    cg.add(var.set_tcp_keepalive_enabled(ka[CONF_TCP_KEEPALIVE_ENABLED]))
    cg.add(var.set_tcp_keepalive_idle_sec(int(ka[CONF_TCP_KEEPALIVE_IDLE].total_seconds)))
    cg.add(
        var.set_tcp_keepalive_interval_sec(
            int(ka[CONF_TCP_KEEPALIVE_INTERVAL].total_seconds)
        )
    )
    cg.add(var.set_tcp_keepalive_count(ka[CONF_TCP_KEEPALIVE_COUNT]))
