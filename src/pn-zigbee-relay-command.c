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

#include "pn-zigbee-relay-command.h"

#include <pn-message.h>

#include <json-glib/json-glib.h>

/* Zigbee2MQTT accepts commands on `zigbee2mqtt/<friendly_name>/set`.
 * The prefix is configurable in Z2M but the default ("zigbee2mqtt") is
 * what almost every install uses; pinned here for parity with the rest
 * of the plugin (#PnZigbeeSource, #PnZigbeeSwitch). */
#define PN_ZIGBEE_BASE_TOPIC "zigbee2mqtt"

/* fa-toggle-on U+F205 -- same glyph the #PnZigbeeSwitch and the host's
 * PnSwitch base use, since this is the write-half of that same role.
 * While the friendly-name field is empty the node flags itself via the
 * host's has-error overlay (red body + ❗), matching the convention
 * #PnZigbeeSwitch / #PnInject use to flag a node that needs
 * configuration before it can do anything -- see PLUGINS §12. */
#define PN_ZIGBEE_RELAY_COMMAND_ICON         "\xef\x88\x85"

struct _PnZigbeeRelayCommand
{
    PnNode parent_instance;

    /* Mandatory.  Stamped into the outgoing envelope topic as
     * zigbee2mqtt/<friendly_name>/set.  An empty / NULL value marks the
     * node as needing setup and rejects every message -- there is no
     * safe default we could pick on the user's behalf, since silently
     * defaulting would risk flipping the wrong physical device. */
    gchar *friendly_name;
};

G_DEFINE_TYPE (PnZigbeeRelayCommand, pn_zigbee_relay_command, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_FRIENDLY_NAME,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Keeps the Zigbee identity (magenta + toggle glyph, matching the rest
 *  of the palette) set unconditionally on the *instance* -- which is
 *  what the worksheet body reads -- and toggles the host's has-error
 *  overlay (red body + ❗) while the friendly-name field is empty, the
 *  way PLUGINS §12 prescribes. */
static void
apply_visual_state (
        PnZigbeeRelayCommand *self,
        gboolean              configured)
{
    PnNode *node = PN_NODE (self);
    PnColor magenta = { 0.78, 0.27, 0.60, 1.0 };

    pn_node_set_color (node, &magenta);
    pn_node_set_icon  (node, PN_ZIGBEE_RELAY_COMMAND_ICON);
    pn_node_set_has_error (node, !configured);

    pn_node_request_repaint (node);
}

/* ------------------------------------------------------------------ */
/*  Payload introspection                                              */
/* ------------------------------------------------------------------ */

/** Read a numeric `data.value` member off @data into @out_value.
 *  Accepts both the double and int64 JSON value types (an Inject emits
 *  a double, a counter an int).  Returns %FALSE when the member is
 *  missing or not numeric, so a message without a usable value leaves
 *  the device alone -- the same rule #PnZigbeeRelayStatus uses on the
 *  read side, so the pair stays internally consistent. */
static gboolean
read_value_member (
        JsonObject *data,
        gdouble    *out_value)
{
    JsonNode *n;
    GType     t;

    if (data == NULL || !json_object_has_member (data, "value"))
        return FALSE;

    n = json_object_get_member (data, "value");
    if (!JSON_NODE_HOLDS_VALUE (n))
        return FALSE;

    t = json_node_get_value_type (n);
    if (t == G_TYPE_DOUBLE)
        *out_value = json_node_get_double (n);
    else if (t == G_TYPE_INT64)
        *out_value = (gdouble) json_node_get_int (n);
    else
        return FALSE;

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive -- build the Zigbee2MQTT `set` command shape               */
/* ------------------------------------------------------------------ */

/** Complete override of the base receive vfunc.  Reshapes the inbound
 *  value message in place into a Z2M `set` command and forwards it.
 *  Mutating in place is safe: the wire layer hands each fan-out branch
 *  its own #PnMessage clone, so this node owns the copy it receives. */
static void
pn_zigbee_relay_command_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnZigbeeRelayCommand *self = PN_ZIGBEE_RELAY_COMMAND (node);
    JsonObject           *payload_obj;
    JsonNode             *payload_node;
    gdouble               value;
    gboolean              on;
    gchar                *topic;
    gchar                *output;

    /* Unconfigured nodes drop every message -- the whole point of the
     * mandatory friendly-name field is that we must never command a
     * physical device without an explicit per-instance target. */
    if (self->friendly_name == NULL || *self->friendly_name == '\0')
        return;

    /* Messages without a usable numeric value leave the device alone --
     * same rule #PnZigbeeRelayStatus uses on the read side, so the pair
     * stays internally consistent. */
    if (!read_value_member (pn_message_get_data (message), &value))
        return;

    on = (value > 0.5);

    /* Rewrite the envelope topic so a downstream PnMqttSink publishes
     * straight to the Z2M command topic without an intervening PnRewrite
     * step.  The plain `zigbee2mqtt/<dev>/set` shape matches what Z2M
     * itself documents and reads cleanly on the Debug pane. */
    topic = g_strdup_printf ("%s/%s/set", PN_ZIGBEE_BASE_TOPIC,
                             self->friendly_name);
    pn_message_set_topic (message, topic);
    g_free (topic);

    /* Structured payload: Z2M's `set` endpoint expects a JSON object
     * (typically `{"state":"ON"}`); #PnMqttSink notices that
     * `data.payload` is structured and serialises it back to JSON bytes
     * on the wire.  Setting it as a JsonNode object (rather than a
     * pre-stringified string) keeps the message downstream-
     * introspectable -- a Debug node sees the structured form, not
     * opaque bytes.  This mirrors #PnZigbeeSwitch's outbound builder so
     * the factored write-half puts exactly the same shape on the wire as
     * the fused switch. */
    payload_obj = json_object_new ();
    json_object_set_string_member (payload_obj, "state", on ? "ON" : "OFF");
    payload_node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (payload_node, payload_obj);
    pn_message_set_member (message, "payload", payload_node);

    /* The original numeric value rides along on data.value so any
     * downstream branch (an LED indicator wired off the same point) can
     * still mirror the on/off state without parsing the payload. */
    pn_message_set_double  (message, "value",   on ? 1.0 : 0.0);
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "device",  self->friendly_name);

    output = g_strdup_printf ("%s: command state %s",
                              self->friendly_name, on ? "ON" : "OFF");
    pn_message_set_string (message, "output", output);
    g_free (output);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
zigbee_relay_command_set_name (
        PnZigbeeRelayCommand *self,
        const gchar          *name)
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
pn_zigbee_relay_command_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnZigbeeRelayCommand *self = PN_ZIGBEE_RELAY_COMMAND (object);

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
pn_zigbee_relay_command_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnZigbeeRelayCommand *self = PN_ZIGBEE_RELAY_COMMAND (object);

    switch (prop_id)
    {
    case PROP_FRIENDLY_NAME:
        zigbee_relay_command_set_name (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject / class plumbing                                           */
/* ------------------------------------------------------------------ */

static void
pn_zigbee_relay_command_finalize (GObject *object)
{
    PnZigbeeRelayCommand *self = PN_ZIGBEE_RELAY_COMMAND (object);

    g_clear_pointer (&self->friendly_name, g_free);

    G_OBJECT_CLASS (pn_zigbee_relay_command_parent_class)->finalize (object);
}

static void
pn_zigbee_relay_command_class_init (PnZigbeeRelayCommandClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_zigbee_relay_command_get_property;
    object_class->set_property = pn_zigbee_relay_command_set_property;
    object_class->finalize     = pn_zigbee_relay_command_finalize;

    node_class->receive = pn_zigbee_relay_command_receive;

    node_class->palette_icon = PN_ZIGBEE_RELAY_COMMAND_ICON;
    node_class->class_name   = "Zigbee Relay Command";
    node_class->icon         = PN_ZIGBEE_RELAY_COMMAND_ICON;
    node_class->color        = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category     = "Zigbee";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_FRIENDLY_NAME] = g_param_spec_string (
            "friendly-name", "Friendly name",
            "Zigbee2MQTT friendly name to command.  Stamped into the "
            "outgoing envelope topic as zigbee2mqtt/<friendly-name>/set "
            "with a JSON {\"state\":\"ON\"|\"OFF\"} payload so a "
            "downstream MQTT Sink publishes straight to the right Zigbee "
            "endpoint.  Empty marks the node as needing configuration and "
            "rejects every message -- mandatory by design, since silently "
            "defaulting would risk flipping the wrong physical device.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_zigbee_relay_command_init (PnZigbeeRelayCommand *self)
{
    self->friendly_name = NULL;

    pn_node_set_has_input  (PN_NODE (self), TRUE);
    pn_node_set_has_output (PN_NODE (self), TRUE);

    /* Start in the unconfigured (red ❗) state so a freshly-dropped node
     * visibly refuses to operate until the user fills in the
     * friendly-name field.  This also seeds the instance icon/color,
     * which the worksheet body paints from (the class fields only feed
     * the palette entry). */
    apply_visual_state (self, FALSE);
}

PnZigbeeRelayCommand *
pn_zigbee_relay_command_new (void)
{
    return g_object_new (PN_TYPE_ZIGBEE_RELAY_COMMAND, NULL);
}
