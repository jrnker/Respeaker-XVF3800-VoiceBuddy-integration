"""ESPHome codegen for the voicebuddy_satellite component.

Speaks BARK (docs/PROTOCOLS.md §5 in the VoiceHA repo) directly to the
VoiceBuddy hub. Replaces the stock `voice_assistant:` + `api:` blocks.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import microphone, speaker
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER, CONF_TRIGGER_ID

DEPENDENCIES = ["network", "socket"]
AUTO_LOAD = ["socket"]
CODEOWNERS = ["@jrnker"]

voicebuddy_ns = cg.esphome_ns.namespace("voicebuddy_satellite")
VoicebuddySatellite = voicebuddy_ns.class_("VoicebuddySatellite", cg.Component)

WakeAction = voicebuddy_ns.class_("WakeAction", automation.Action)
StartListeningAction = voicebuddy_ns.class_("StartListeningAction", automation.Action)
StopListeningAction = voicebuddy_ns.class_("StopListeningAction", automation.Action)
SetConfigAction = voicebuddy_ns.class_("SetConfigAction", automation.Action)
FactoryResetAction = voicebuddy_ns.class_("FactoryResetAction", automation.Action)

OnConnectedTrigger = voicebuddy_ns.class_("OnConnectedTrigger", automation.Trigger.template())
OnDisconnectedTrigger = voicebuddy_ns.class_("OnDisconnectedTrigger", automation.Trigger.template())
OnTtsStartTrigger = voicebuddy_ns.class_("OnTtsStartTrigger", automation.Trigger.template())
OnTtsEndTrigger = voicebuddy_ns.class_("OnTtsEndTrigger", automation.Trigger.template())

CONF_HUB_HOST = "hub_host"
CONF_HUB_PORT = "hub_port"
CONF_ROOM_ID = "room_id"
CONF_SATELLITE_ID = "satellite_id"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_ON_TTS_START = "on_tts_start"
CONF_ON_TTS_END = "on_tts_end"
CONF_WAKE_ID = "wake_id"
CONF_CONFIDENCE = "confidence"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(VoicebuddySatellite),
    # All four identity fields used to be `cv.Required` — that wired the
    # values straight into compiled C++ via `cg.add(set_hub(...))`. The
    # provisioning flow pushes them in at boot from NVS-backed config
    # entities instead, so they're optional at compile time. Anything left
    # unset here can still be supplied via `voicebuddy_satellite.set_config`
    # (see the action below) before connect is attempted.
    cv.Optional(CONF_HUB_HOST, default=""): cv.string_strict,
    cv.Optional(CONF_HUB_PORT, default=9102): cv.port,
    cv.Optional(CONF_ROOM_ID, default=""): cv.All(cv.string_strict, cv.Length(max=16)),
    cv.Optional(CONF_SATELLITE_ID, default=""): cv.All(cv.string_strict, cv.Length(max=16)),
    cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=1,
    ),
    cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
    cv.Optional(CONF_ON_CONNECTED): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnConnectedTrigger),
    }),
    cv.Optional(CONF_ON_DISCONNECTED): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnDisconnectedTrigger),
    }),
    cv.Optional(CONF_ON_TTS_START): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnTtsStartTrigger),
    }),
    cv.Optional(CONF_ON_TTS_END): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnTtsEndTrigger),
    }),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # These will be empty strings on a freshly-flashed satellite that's
    # provisioned at runtime; the runtime path calls set_config_runtime()
    # before connect is attempted. For the legacy compile-time-substitution
    # config (voicebuddy-satellite-minimal.yaml) the values are real here
    # and connect proceeds immediately on boot just like before.
    cg.add(var.set_hub(config[CONF_HUB_HOST], config[CONF_HUB_PORT]))
    cg.add(var.set_room_id(config[CONF_ROOM_ID]))
    if config.get(CONF_SATELLITE_ID):
        cg.add(var.set_satellite_id(config[CONF_SATELLITE_ID]))

    mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone_source(mic_source))
    if CONF_SPEAKER in config:
        spk = await cg.get_variable(config[CONF_SPEAKER])
        cg.add(var.set_speaker(spk))

    for conf in config.get(CONF_ON_CONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_DISCONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_TTS_START, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_TTS_END, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


# --- Automation actions ----------------------------------------------

WAKE_ACTION_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(VoicebuddySatellite),
    cv.Optional(CONF_WAKE_ID, default=0): cv.templatable(cv.uint8_t),
    cv.Optional(CONF_CONFIDENCE, default=255): cv.templatable(cv.uint8_t),
})


@automation.register_action(
    "voicebuddy_satellite.wake", WakeAction, WAKE_ACTION_SCHEMA, synchronous=False,
)
async def wake_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    wake_id = await cg.templatable(config[CONF_WAKE_ID], args, cg.uint8)
    conf = await cg.templatable(config[CONF_CONFIDENCE], args, cg.uint8)
    cg.add(var.set_wake_id(wake_id))
    cg.add(var.set_confidence(conf))
    return var


SIMPLE_ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(VoicebuddySatellite)})


@automation.register_action(
    "voicebuddy_satellite.start_listening",
    StartListeningAction,
    SIMPLE_ACTION_SCHEMA,
    synchronous=False,
)
async def start_listening_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "voicebuddy_satellite.stop_listening",
    StopListeningAction,
    SIMPLE_ACTION_SCHEMA,
    synchronous=False,
)
async def stop_listening_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


# --- Runtime config push ---------------------------------------------
#
# `voicebuddy_satellite.set_config` lets the YAML push hub_host/hub_port/
# room_id/satellite_id into the component at run-time. Used by the
# provisioning flow (see config/voicebuddy-satellite-provisioned.yaml):
# on_boot reads NVS-backed `text:` / `number:` config entities and feeds
# them in here before WiFi finishes connecting; the component then walks
# from DISCONNECTED → CONNECTING just like the compile-time path.

SET_CONFIG_ACTION_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(VoicebuddySatellite),
    cv.Required(CONF_HUB_HOST): cv.templatable(cv.string),
    cv.Optional(CONF_HUB_PORT, default=9102): cv.templatable(cv.port),
    cv.Required(CONF_ROOM_ID): cv.templatable(cv.string),
    cv.Optional(CONF_SATELLITE_ID, default=""): cv.templatable(cv.string),
})


@automation.register_action(
    "voicebuddy_satellite.set_config",
    SetConfigAction,
    SET_CONFIG_ACTION_SCHEMA,
    synchronous=False,
)
async def set_config_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    host = await cg.templatable(config[CONF_HUB_HOST], args, cg.std_string)
    port = await cg.templatable(config[CONF_HUB_PORT], args, cg.uint16)
    room = await cg.templatable(config[CONF_ROOM_ID], args, cg.std_string)
    sat = await cg.templatable(config[CONF_SATELLITE_ID], args, cg.std_string)
    cg.add(var.set_hub_host(host))
    cg.add(var.set_hub_port(port))
    cg.add(var.set_room_id_value(room))
    cg.add(var.set_satellite_id_value(sat))
    return var


# `voicebuddy_satellite.factory_reset` clears the live config and stops
# any running session so the device drops back into provisioning mode at
# the next boot. The captive-portal flow calls this from a long-press
# gesture on BUT_A; the persisted text/number entities themselves are
# wiped via global.set on the YAML side because that's where the NVS
# handles live.

@automation.register_action(
    "voicebuddy_satellite.factory_reset",
    FactoryResetAction,
    SIMPLE_ACTION_SCHEMA,
    synchronous=False,
)
async def factory_reset_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
