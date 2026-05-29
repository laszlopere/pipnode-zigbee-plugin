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

#include "pn-zigbee-relay-status.h"

#include <pn-message.h>

#include <json-glib/json-glib.h>

/* Zigbee2MQTT publishes endpoint state on `zigbee2mqtt/<friendly_name>`.
 * The prefix is configurable in Z2M but the default ("zigbee2mqtt") is
 * what almost every install uses; pinned here for parity with the rest
 * of the plugin (#PnZigbeeSource, #PnZigbeeSwitch). */
#define PN_ZIGBEE_BASE_TOPIC "zigbee2mqtt"

/* fa-toggle-on U+F205 -- same glyph the #PnZigbeeSwitch and the host's
 * PnSwitch base use, since this is the read-half of that same role.
 * Swapped to the warning ❗ glyph (U+2757) while the friendly-name field
 * is empty, matching the convention #PnZigbeeSwitch / #PnInject use to
 * flag a node that needs configuration before it can do anything. */
#define PN_ZIGBEE_RELAY_STATUS_ICON         "\xef\x88\x85"
#define PN_ZIGBEE_RELAY_STATUS_WARNING_ICON "\xe2\x9d\x97"

struct _PnZigbeeRelayStatus
{
    PnNode parent_instance;

    /* Zigbee2MQTT friendly name the node filters on.  Only publishes
     * whose envelope topic is exactly zigbee2mqtt/<friendly_name> (the
     * canonical Z2M state topic, no /set or /availability suffix) are
     * decoded; everything else is dropped silently.  An empty / NULL
     * value marks the node as unconfigured and rejects every message --
     * there is no sensible default device we could pick on the user's
     * behalf. */
    gchar *friendly_name;
};

G_DEFINE_TYPE (PnZigbeeRelayStatus, pn_zigbee_relay_status, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_FRIENDLY_NAME,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Magenta + toggle glyph when the friendly-name field is set (matches
 *  the rest of the Zigbee palette), red + ❗ glyph when not.  Paints the
 *  *instance* icon/color, which is what the worksheet body reads (the
 *  class fields only seed the palette entry). */
static void
apply_visual_state (
        PnZigbeeRelayStatus *self,
        gboolean             configured)
{
    PnNode *node = PN_NODE (self);

    if (configured)
    {
        PnColor magenta = { 0.78, 0.27, 0.60, 1.0 };
        pn_node_set_color (node, &magenta);
        pn_node_set_icon  (node, PN_ZIGBEE_RELAY_STATUS_ICON);
    }
    else
    {
        PnColor red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_ZIGBEE_RELAY_STATUS_WARNING_ICON);
    }

    pn_node_request_repaint (node);
}

/* ------------------------------------------------------------------ */
/*  Payload introspection                                              */
/* ------------------------------------------------------------------ */

/** Topic filter: does @topic exactly match this device's state topic?
 *  Z2M publishes the full state object on the bare friendly-name path
 *  (per-feature sub-topics like /availability or /set exist but the
 *  state lives at the bare path).  Exact-match so a sibling endpoint
 *  named "lamp" does not steal publishes destined for "lamp_bedroom". */
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

/** Pull a string `data.payload.state` value off @message.  Z2M publishes
 *  a JSON object payload (`{"state":"ON","linkquality":...}`), so
 *  `data.payload` is itself a structured JsonNode object rather than the
 *  bare-string form Tasmota uses.  Returns a borrowed pointer into the
 *  payload (valid while @message is alive) or %NULL when the payload has
 *  no usable string `state` member. */
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

/* ------------------------------------------------------------------ */
/*  Receive -- decode, reshape, forward                                */
/* ------------------------------------------------------------------ */

/** Complete override of the base receive vfunc.  Drops every message
 *  that is not a state publish for the configured device; for the
 *  survivor it stamps the canonical pipnode contract onto the message
 *  in place and forwards it.  Mutating in place is safe: the wire layer
 *  hands each fan-out branch its own #PnMessage clone, so this node owns
 *  the copy it receives.
 *
 *  Unlike #PnZigbeeSwitch's inbound path, this node *does* emit: it is a
 *  pure observer with no latch to sync, so forwarding the decoded value
 *  is its whole job.  There is no feedback-loop risk because it never
 *  rewrites the topic to a `set` command -- the output is a value
 *  message for observers (LED / Debug / Graph), not a device command. */
static void
pn_zigbee_relay_status_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnZigbeeRelayStatus *self = PN_ZIGBEE_RELAY_STATUS (node);
    const gchar         *state;
    const gchar         *topic;
    gboolean             on;
    gchar               *output;

    /* Unconfigured nodes drop every message -- there is no sensible
     * default device name we could pick on the user's behalf. */
    if (self->friendly_name == NULL || *self->friendly_name == '\0')
        return;

    topic = pn_message_get_topic (message);
    if (!topic_matches_zigbee_state (topic, self->friendly_name))
        return;

    state = get_payload_state_string (message);
    if (state == NULL)
        return;

    /* Only the literal "ON" / "OFF" strings yield a state; Z2M can also
     * emit "TOGGLE" through some converters, and richer endpoints
     * publish state objects with no top-level "state" member at all --
     * both fall through here.  Silently dropping a non-binary state is
     * friendlier than synthesising a 0 that downstream filters would
     * then treat as a real off-transition. */
    if (g_ascii_strcasecmp (state, "ON") == 0)
        on = TRUE;
    else if (g_ascii_strcasecmp (state, "OFF") == 0)
        on = FALSE;
    else
        return;

    /* Reshape into the canonical pipnode message contract so a
     * downstream LED / Debug / Graph reads the same value/success/output
     * shape it gets from any other node.  The original envelope and
     * `data.payload` (linkquality, brightness, etc.) are left intact for
     * nodes that want the detail. */
    pn_message_set_double  (message, "value",   on ? 1.0 : 0.0);
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "device",  self->friendly_name);

    output = g_strdup_printf ("%s: state %s",
                              self->friendly_name, on ? "ON" : "OFF");
    pn_message_set_string (message, "output", output);
    g_free (output);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
zigbee_relay_status_set_name (
        PnZigbeeRelayStatus *self,
        const gchar         *name)
{
    /* Normalise NULL / "" so both forms produce the same unconfigured
     * state, but round-trip whatever the caller passed so the dialog
     * sees a stable value on notify::friendly-name. */
    gchar    *replacement = (name != NULL) ? g_strdup (name) : NULL;
    gboolean  configured;

    g_free (self->friendly_name);
    self->friendly_name = replacement;

    configured = (self->friendly_name != NULL && *self->friendly_name != '\0');
    apply_visual_state (self, configured);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_FRIENDLY_NAME]);
}

static void
pn_zigbee_relay_status_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnZigbeeRelayStatus *self = PN_ZIGBEE_RELAY_STATUS (object);

    switch (prop_id)
    {
    case PROP_FRIENDLY_NAME:
        g_value_set_string (value, self->friendly_name);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_zigbee_relay_status_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnZigbeeRelayStatus *self = PN_ZIGBEE_RELAY_STATUS (object);

    switch (prop_id)
    {
    case PROP_FRIENDLY_NAME:
        zigbee_relay_status_set_name (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject / class plumbing                                           */
/* ------------------------------------------------------------------ */

static void
pn_zigbee_relay_status_finalize (GObject *object)
{
    PnZigbeeRelayStatus *self = PN_ZIGBEE_RELAY_STATUS (object);

    g_clear_pointer (&self->friendly_name, g_free);

    G_OBJECT_CLASS (pn_zigbee_relay_status_parent_class)->finalize (object);
}

static void
pn_zigbee_relay_status_class_init (PnZigbeeRelayStatusClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_zigbee_relay_status_get_property;
    object_class->set_property = pn_zigbee_relay_status_set_property;
    object_class->finalize     = pn_zigbee_relay_status_finalize;

    node_class->receive = pn_zigbee_relay_status_receive;

    node_class->palette_icon = PN_ZIGBEE_RELAY_STATUS_ICON;
    node_class->class_name   = "Zigbee Relay Status";
    node_class->icon         = PN_ZIGBEE_RELAY_STATUS_ICON;
    node_class->color        = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category     = "Zigbee";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_FRIENDLY_NAME] = g_param_spec_string (
            "friendly-name", "Friendly name",
            "Zigbee2MQTT friendly name the filter listens for.  Only "
            "publishes whose envelope topic is exactly "
            "zigbee2mqtt/<friendly-name> (the canonical Z2M state topic, "
            "no /set or /availability suffix) are decoded; the top-level "
            "\"state\" member of the JSON payload is read as ON / OFF and "
            "re-emitted as the canonical data.value (1.0 / 0.0) shape.  "
            "Empty marks the node as needing configuration and rejects "
            "every message -- there is no sensible default device we "
            "could pick on the user's behalf.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_zigbee_relay_status_init (PnZigbeeRelayStatus *self)
{
    self->friendly_name = NULL;

    pn_node_set_has_input  (PN_NODE (self), TRUE);
    pn_node_set_has_output (PN_NODE (self), TRUE);

    /* Start in the unconfigured (red ❗) state so a freshly-dropped node
     * visibly nags the user to fill the friendly-name field.  This also
     * seeds the instance icon/color, which the worksheet body paints
     * from (the class fields only feed the palette entry). */
    apply_visual_state (self, FALSE);
}

PnZigbeeRelayStatus *
pn_zigbee_relay_status_new (void)
{
    return g_object_new (PN_TYPE_ZIGBEE_RELAY_STATUS, NULL);
}
