/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 *
 * This file is part of the pipnode-zigbee-plugin, a proprietary plugin
 * for Pipnode.  It links the pipnode host library solely through the
 * documented plugin interface (pn_plugin_init and the public pipnode
 * headers) and is therefore distributed under the additional permission
 * granted by Pipnode's LICENSE.PLUGIN-EXCEPTION, which allows a plugin
 * to be released under any license, including this proprietary one.
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-zigbee-switch.h"

#include <pn-message.h>

#include <json-glib/json-glib.h>

/* Zigbee2MQTT publishes endpoint state on `zigbee2mqtt/<friendly_name>`
 * and accepts commands on `zigbee2mqtt/<friendly_name>/set`.  The prefix
 * is configurable in Z2M but the default ("zigbee2mqtt") is what almost
 * every install uses; we pin it here for parity with the Tasmota switch
 * (which similarly pins "cmnd" / "stat") and to keep the property dialog
 * down to a single field.  A user on a non-default prefix can either
 * rename their Z2M base topic or wire the explicit, factored chain. */
#define PN_ZIGBEE_BASE_TOPIC "zigbee2mqtt"

/* fa-toggle-on U+F205 -- same glyph the host's PnSwitch base and the
 * Tasmota switch use, since this is the same role on a different
 * transport.  While the friendly-name field is empty the node flags
 * itself via the host's has-error overlay (red body + ❗), the way
 * #PnInject marks a node that needs configuration before it can do
 * anything useful -- see PLUGINS §12. */
#define PN_ZIGBEE_SWITCH_ICON         "\xef\x88\x85"

struct _PnZigbeeSwitch
{
    PnSwitch parent_instance;

    /* Zigbee2MQTT friendly name of the device the node binds to.
     * Inbound: only publishes on `zigbee2mqtt/<friendly-name>` (exact
     * topic match, no suffix) update the latch.  Outbound: stamped
     * into `zigbee2mqtt/<friendly-name>/set`.  Mandatory: an empty
     * value rejects every inbound message and refuses to emit on
     * user-click, since silently defaulting would risk reading or
     * flipping the wrong physical device. */
    gchar *friendly_name;
};

G_DEFINE_TYPE (PnZigbeeSwitch, pn_zigbee_switch, PN_TYPE_SWITCH)

enum {
    PROP_0,
    PROP_FRIENDLY_NAME,
    PROP_ENFORCE_ON_STARTUP,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Override of the base #PnSwitch vfunc.  Keeps the Zigbee identity
 *  (magenta + toggle glyph, matching the shared Zigbee palette) set
 *  unconditionally and toggles the host's has-error
 *  overlay (red body + ❗) while the friendly-name field is empty, the
 *  way PLUGINS §12 prescribes -- no hand-rolled colour/icon swap.
 *  Repaint is still needed for the slider widget to redraw, so we keep
 *  the base contract. */
static void
pn_zigbee_switch_apply_visual_state (PnSwitch *base)
{
    PnZigbeeSwitch *self = PN_ZIGBEE_SWITCH (base);
    PnNode         *node = PN_NODE (self);
    PnColor         magenta = { 0.78, 0.27, 0.60, 1.0 };
    gboolean        configured;

    configured = (self->friendly_name != NULL && *self->friendly_name != '\0');

    pn_node_set_color (node, &magenta);
    pn_node_set_icon  (node, PN_ZIGBEE_SWITCH_ICON);
    pn_node_set_has_error (node, !configured);

    pn_node_request_repaint (node);
}

/* ------------------------------------------------------------------ */
/*  Outbound -- Zigbee2MQTT `set` command shape                        */
/* ------------------------------------------------------------------ */

/** Override of the base #PnSwitch vfunc.  Build a Z2M command message:
 *  envelope topic = `zigbee2mqtt/<friendly_name>/set`, `data.payload`
 *  a structured JSON object `{ "state": "ON" | "OFF" }` (the
 *  #PnMqttSink serialises structured payload members back to JSON for
 *  the wire), plus the standard `data.value` / `data.success` /
 *  `data.device` / `data.output` mirror so a downstream LED / Debug /
 *  Graph reads the same shape regardless of which transport the
 *  click was encoded for. */
static PnMessage *
pn_zigbee_switch_build_outbound_message (PnSwitch *base)
{
    PnZigbeeSwitch *self = PN_ZIGBEE_SWITCH (base);
    PnNode         *node = PN_NODE (self);
    PnMessage      *msg;
    JsonObject     *payload_obj;
    JsonNode       *payload_node;
    gboolean        on;
    gchar          *topic;
    gchar          *output;

    on = pn_switch_get_on (base);

    /* Unconfigured nodes still build a message (the dispatcher will
     * emit it) but without a friendly_name there is nothing useful
     * to stamp -- fall back to the base shape so a misconfigured
     * node does not silently publish to an empty
     * `zigbee2mqtt//set` topic.  The red ❗ visual already nags the
     * user to configure the field. */
    if (self->friendly_name == NULL || *self->friendly_name == '\0')
    {
        pn_node_log_warning (node,
                             "no friendly_name configured; emitting the base "
                             "switch shape instead of a zigbee2mqtt set command");
        return pn_switch_real_build_outbound_message (base);
    }

    msg   = pn_message_new (node, NULL);
    topic = g_strdup_printf ("%s/%s/set", PN_ZIGBEE_BASE_TOPIC,
                             self->friendly_name);
    pn_message_set_topic (msg, topic);
    g_free (topic);

    /* Structured payload: Z2M's `set` endpoint expects a JSON object
     * (typically `{"state":"ON"}`); #PnMqttSink notices that
     * `data.payload` is structured and serialises it back to JSON
     * bytes on the wire.  Setting it as a JsonNode object (rather
     * than a pre-stringified string) keeps the message
     * downstream-introspectable -- a Debug node sees the structured
     * form, not opaque bytes. */
    payload_obj = json_object_new ();
    json_object_set_string_member (payload_obj, "state", on ? "ON" : "OFF");
    payload_node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (payload_node, payload_obj);
    pn_message_set_member (msg, "payload", payload_node);

    pn_message_set_double  (msg, "value",   on ? 1.0 : 0.0);
    pn_message_set_boolean (msg, "success", TRUE);
    pn_message_set_string  (msg, "device",  self->friendly_name);

    output = g_strdup_printf ("%s: command state %s",
                              self->friendly_name, on ? "ON" : "OFF");
    pn_message_set_string (msg, "output", output);
    g_free (output);

    return msg;
}

/* ------------------------------------------------------------------ */
/*  Inbound -- Z2M state decode, latch sync silently                   */
/* ------------------------------------------------------------------ */

/** Z2M publishes a device's full state on the bare friendly-name
 *  topic (`zigbee2mqtt/<friendly_name>`).  Per-feature sub-topics
 *  exist (`/availability`, `/set`, etc.) but the state object lives
 *  at the bare path.  Exact-match here so a sibling endpoint named
 *  "lamp" does not steal publishes destined for "lamp_bedroom". */
static gboolean
topic_matches_zigbee_state (
        const gchar *topic,
        const gchar *friendly_name)
{
    gchar    *expected;
    gboolean  match;

    if (topic == NULL || *topic == '\0' ||
        friendly_name == NULL || *friendly_name == '\0')
        return FALSE;

    expected = g_strdup_printf ("%s/%s", PN_ZIGBEE_BASE_TOPIC, friendly_name);
    match    = (g_strcmp0 (topic, expected) == 0);
    g_free (expected);
    return match;
}

/** Pull a string `data.payload.state` value off @message.  Z2M
 *  publishes a JSON object payload (`{"state":"ON","linkquality":...}`),
 *  so `data.payload` is itself a structured JsonNode object rather
 *  than the bare-string form Tasmota uses.  Returns a borrowed
 *  pointer into the payload (valid for as long as @message is alive)
 *  or %NULL when the payload has no usable string `state` member. */
static const gchar *
get_payload_state_string (PnMessage *message)
{
    JsonNode   *payload_node;
    JsonObject *payload_obj;
    JsonNode   *state_node;

    payload_node = pn_message_get_member (message, "payload");
    if (payload_node == NULL || !JSON_NODE_HOLDS_OBJECT (payload_node))
        return NULL;

    payload_obj = json_node_get_object (payload_node);
    if (payload_obj == NULL)
        return NULL;
    if (!json_object_has_member (payload_obj, "state"))
        return NULL;

    state_node = json_object_get_member (payload_obj, "state");
    if (state_node == NULL || !JSON_NODE_HOLDS_VALUE (state_node))
        return NULL;
    if (json_node_get_value_type (state_node) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string (state_node);
}

/** Complete override of the base receive vfunc (does *not* chain to
 *  parent).  An inbound Z2M state publish authoritatively describes
 *  the device's current state; we mirror it on the latch by writing
 *  the "on" property (which dispatches the apply_visual_state vfunc
 *  and notifies, the same path a programmatic set goes through) and
 *  stop.  Nothing is emitted downstream because:
 *
 *    * The inbound is a raw MQTT envelope -- forwarding it would
 *      leak broker bytes into nodes (LEDs, graphs) that expect the
 *      canonical data.value shape.
 *
 *    * Even after decoding, re-emitting would close a loop straight
 *      back through PnMqttSink to the broker, commanding the device
 *      to do what it already does.  The base PnSwitch's latch-
 *      equality storm-breaker is implicit here: if the inbound state
 *      matches the current latch, nothing happens; if it differs, we
 *      only update internal state, never emit.  Either way the cycle
 *      cannot form. */
static void
pn_zigbee_switch_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnZigbeeSwitch *self = PN_ZIGBEE_SWITCH (node);
    PnSwitch       *base = PN_SWITCH (node);
    const gchar    *state;
    const gchar    *topic;
    gboolean        want_on;

    if (self->friendly_name == NULL || *self->friendly_name == '\0')
        return;

    topic = pn_message_get_topic (message);
    if (!topic_matches_zigbee_state (topic, self->friendly_name))
        return;

    state = get_payload_state_string (message);
    if (state == NULL)
        return;

    /* Only the literal "ON" / "OFF" strings move the latch; Z2M can
     * also emit "TOGGLE" through some converters, and richer endpoints
     * publish state objects that have no top-level "state" member at
     * all -- both fall through here so a noise burst cannot synthesise
     * a false transition. */
    if (g_ascii_strcasecmp (state, "ON") == 0)
        want_on = TRUE;
    else if (g_ascii_strcasecmp (state, "OFF") == 0)
        want_on = FALSE;
    else
    {
        /* A present-but-non-binary state (e.g. "TOGGLE") is worth a
         * line; the no-state-member case is filtered upstream as
         * state == NULL and stays quiet, since Z2M publishes every
         * attribute update on this same topic (PLUGINS §12, channel 3). */
        pn_node_log_info (node, "ignoring unrecognized state \"%s\" for %s",
                          state, self->friendly_name);
        return;
    }

    if (want_on != pn_switch_get_on (base))
    {
        /* g_object_set on the "on" property goes through the base
         * class's set_property, which updates internal state, calls
         * the apply_visual_state vfunc (dispatching to our override)
         * and notifies listeners -- without emitting.  Exactly the
         * silent-latch-sync semantics this branch needs. */
        g_object_set (base, "on", want_on, NULL);
    }
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
zigbee_switch_set_name (
        PnZigbeeSwitch *self,
        const gchar    *name)
{
    /* Normalise NULL / "" so both forms produce the same unconfigured
     * state, but round-trip whatever the caller passed so the dialog
     * sees a stable value on notify::friendly-name. */
    gchar *replacement = (name != NULL) ? g_strdup (name) : NULL;

    g_free (self->friendly_name);
    self->friendly_name = replacement;

    /* The visual cue (magenta vs red ❗) depends on friendly_name,
     * not on the latch position, so a name change has to re-run the
     * vfunc.  Going through the base class's apply_visual_state hook
     * keeps the dispatch consistent with everything else. */
    PN_SWITCH_GET_CLASS (self)->apply_visual_state (PN_SWITCH (self));

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_FRIENDLY_NAME]);
}

static void
pn_zigbee_switch_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnZigbeeSwitch *self = PN_ZIGBEE_SWITCH (object);

    switch (prop_id)
    {
    case PROP_FRIENDLY_NAME:
        g_value_set_string (value, self->friendly_name);
        break;
    case PROP_ENFORCE_ON_STARTUP:
        /* Backed directly by the base #PnSwitch announce flag -- the
         * property *is* the switch's startup-announce control, just
         * surfaced with a device-flavoured name. */
        g_value_set_boolean (value,
                pn_switch_get_announce_on_startup (PN_SWITCH (object)));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_zigbee_switch_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnZigbeeSwitch *self = PN_ZIGBEE_SWITCH (object);

    switch (prop_id)
    {
    case PROP_FRIENDLY_NAME:
        zigbee_switch_set_name (self, g_value_get_string (value));
        break;
    case PROP_ENFORCE_ON_STARTUP:
    {
        gboolean enforce = g_value_get_boolean (value);
        if (pn_switch_get_announce_on_startup (PN_SWITCH (self)) != enforce)
        {
            pn_switch_set_announce_on_startup (PN_SWITCH (self), enforce);
            g_object_notify_by_pspec (object, props[PROP_ENFORCE_ON_STARTUP]);
        }
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_zigbee_switch_finalize (GObject *object)
{
    PnZigbeeSwitch *self = PN_ZIGBEE_SWITCH (object);

    g_clear_pointer (&self->friendly_name, g_free);

    G_OBJECT_CLASS (pn_zigbee_switch_parent_class)->finalize (object);
}

static void
pn_zigbee_switch_class_init (PnZigbeeSwitchClass *klass)
{
    GObjectClass  *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass   *node_class   = PN_NODE_CLASS (klass);
    PnSwitchClass *switch_class = PN_SWITCH_CLASS (klass);

    object_class->get_property = pn_zigbee_switch_get_property;
    object_class->set_property = pn_zigbee_switch_set_property;
    object_class->finalize     = pn_zigbee_switch_finalize;

    /* Complete override of the base receive vfunc (no chaining). */
    node_class->receive = pn_zigbee_switch_receive;

    /* Hook into the two #PnSwitch extension points so the slider
     * widget, hit-testing and the worksheet's click routing keep
     * working unmodified while the visual scheme and the outbound
     * message shape become Zigbee-flavoured. */
    switch_class->apply_visual_state     = pn_zigbee_switch_apply_visual_state;
    switch_class->build_outbound_message = pn_zigbee_switch_build_outbound_message;

    node_class->palette_icon = PN_ZIGBEE_SWITCH_ICON;
    node_class->class_name   = "Zigbee Switch";
    node_class->icon         = PN_ZIGBEE_SWITCH_ICON;
    node_class->color        = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category     = "Zigbee";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_FRIENDLY_NAME] = g_param_spec_string (
            "friendly-name", "Friendly name",
            "Zigbee2MQTT friendly name the node binds to.  Inbound: "
            "only publishes whose envelope topic is exactly "
            "zigbee2mqtt/<friendly-name> (the canonical Z2M state "
            "topic, no /set or /availability suffix) sync the latch.  "
            "Outbound: stamped into the command envelope as "
            "zigbee2mqtt/<friendly-name>/set with a JSON "
            "{\"state\":\"ON\"|\"OFF\"} payload so a downstream MQTT "
            "Sink publishes straight to the right Zigbee endpoint.  "
            "Empty marks the node as needing configuration and rejects "
            "every inbound message -- mandatory by design, since "
            "silently defaulting would risk reading or flipping the "
            "wrong physical device.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ENFORCE_ON_STARTUP] = g_param_spec_boolean (
            "enforce-on-startup", "Enforce on startup",
            "When TRUE, the node commands the device to its saved latch "
            "position once shortly after the worksheet loads, emitting a "
            "zigbee2mqtt/<friendly-name>/set message "
            "({\"state\":\"ON\"|\"OFF\"}) so a downstream MQTT Sink forces "
            "the physical device to match the worksheet on startup.  "
            "Default FALSE: opening a worksheet never actuates the device "
            "-- the latch instead tracks the device's reported state via "
            "inbound zigbee2mqtt/<friendly-name> publishes.  Enabling this "
            "is the right choice when the worksheet, not the device, is "
            "the source of truth for the desired state.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_zigbee_switch_init (PnZigbeeSwitch *self)
{
    self->friendly_name = NULL;

    /* Default the base #PnSwitch startup announce OFF: our
     * build_outbound_message() emits a Z2M *set* command
     * (zigbee2mqtt/<friendly_name>/set = {"state":"ON"|"OFF"}), so
     * announcing on load would actuate the physical device every time
     * a worksheet opens.  The safe default is to leave the device
     * alone and let its true state arrive via inbound
     * zigbee2mqtt/<friendly_name> publishes (see
     * pn_zigbee_switch_receive).  Users who do want the worksheet to
     * force the device to its saved state on load opt back in through
     * the "enforce-on-startup" property, which drives this same
     * flag. */
    pn_switch_set_announce_on_startup (PN_SWITCH (self), FALSE);

    /* Start in the unconfigured (red ❗) state so a freshly-dropped
     * node visibly refuses to operate until the user fills in the
     * friendly-name field.  apply_visual_state inspects friendly_name
     * directly, so this picks up the correct red glyph. */
    PN_SWITCH_GET_CLASS (self)->apply_visual_state (PN_SWITCH (self));
}

PnZigbeeSwitch *
pn_zigbee_switch_new (void)
{
    return g_object_new (PN_TYPE_ZIGBEE_SWITCH, NULL);
}
