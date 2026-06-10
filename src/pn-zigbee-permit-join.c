/*
 * Copyright (C) 2026 Laszlo Pere
 *
 * This file is part of pipnode-zigbee-plugin, a plugin for Pipnode.  It
 * is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 3, or (at your option)
 * any later version, as published by the Free Software Foundation.
 *
 * The plugin links the Pipnode host solely through the documented plugin
 * interface and is covered by Pipnode's LICENSE.PLUGIN-EXCEPTION; this
 * file's own license is the GPL, version 3 or later.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; see the GNU General Public License for more
 * details.  You should have received a copy of the license in the file
 * COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-zigbee-permit-join.h"

#include <pn-message.h>

#include <json-glib/json-glib.h>

/* Zigbee2MQTT exposes bridge controls on `zigbee2mqtt/bridge/request/<cmd>`.
 * The "permit_join" request opens the pairing window.  The prefix is
 * configurable in Z2M but the default ("zigbee2mqtt") is what almost
 * every install uses; pinned here for parity with the rest of the plugin
 * (#PnZigbeeRelayCommand, #PnZigbeeSource, #PnZigbeeSwitch). */
#define PN_ZIGBEE_BASE_TOPIC "zigbee2mqtt"

/* fa-plus-circle U+F055 -- a "add a device" glyph for the join window.
 * Magenta body, matching the rest of the Zigbee palette
 * (#PnZigbeeRelayCommand / #PnZigbeeWaterLeak). */
#define PN_ZIGBEE_PERMIT_JOIN_ICON "\xef\x81\x95"

/* Default pairing window, in minutes.  Four minutes is comfortably
 * longer than the ~60 s a typical sensor needs to pair, while still
 * closing on its own so the network does not sit open indefinitely. */
#define PN_ZIGBEE_PERMIT_JOIN_DEFAULT_MINUTES 4

struct _PnZigbeePermitJoin
{
    PnNode parent_instance;

    /* How long to hold the join window open, in MINUTES.  Converted to
     * the SECONDS Z2M expects (minutes * 60) when the request payload is
     * built.  A non-positive value is clamped to the default. */
    guint minutes;
};

G_DEFINE_TYPE (PnZigbeePermitJoin, pn_zigbee_permit_join, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_MINUTES,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Receive -- build the Z2M bridge permit_join request shape          */
/* ------------------------------------------------------------------ */

/** Complete override of the base receive vfunc.  Treats the inbound
 *  message purely as a trigger and reshapes it in place into a Z2M
 *  `bridge/request/permit_join` command, then forwards it.  Mutating in
 *  place is safe: the wire layer hands each fan-out branch its own
 *  #PnMessage clone, so this node owns the copy it receives. */
static void
pn_zigbee_permit_join_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnZigbeePermitJoin *self = PN_ZIGBEE_PERMIT_JOIN (node);
    JsonObject         *payload_obj;
    JsonNode           *payload_node;
    guint               minutes;
    gint64              seconds;
    gchar              *topic;
    gchar              *output;

    minutes = (self->minutes > 0)
            ? self->minutes
            : PN_ZIGBEE_PERMIT_JOIN_DEFAULT_MINUTES;
    seconds = (gint64) minutes * 60;

    /* Rewrite the envelope topic so a downstream PnMqttSink publishes
     * straight to the Z2M bridge request topic without an intervening
     * PnRewrite step.  The plain `zigbee2mqtt/bridge/request/permit_join`
     * shape is exactly what Z2M documents. */
    topic = g_strdup_printf ("%s/bridge/request/permit_join",
                             PN_ZIGBEE_BASE_TOPIC);
    pn_message_set_topic (message, topic);
    g_free (topic);

    /* Structured payload: Z2M's permit_join request expects a JSON
     * object.  The modern `{"value":true,"time":N}` form is a superset
     * of the older bare `{"time":N}` that Z2M still accepts.  Setting it
     * as a JsonNode object (rather than a pre-stringified string) lets
     * #PnMqttSink serialise it to JSON bytes on the wire while keeping
     * the message downstream-introspectable -- a Debug node sees the
     * structured form.  This mirrors #PnZigbeeRelayCommand's outbound
     * builder exactly. */
    payload_obj = json_object_new ();
    json_object_set_boolean_member (payload_obj, "value", TRUE);
    json_object_set_int_member     (payload_obj, "time",  seconds);
    payload_node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (payload_node, payload_obj);
    pn_message_set_member (message, "payload", payload_node);

    /* Stamp the canonical pipnode value mirror so a parallel observer
     * wired off the same point (an LED, a Debug) reads a value form it
     * recognises -- same convention #PnZigbeeRelayCommand follows. */
    pn_message_set_double  (message, "value",   (gdouble) seconds);
    pn_message_set_boolean (message, "success", TRUE);

    output = g_strdup_printf ("opening join window for %u min (%" G_GINT64_FORMAT " s)",
                              minutes, seconds);
    pn_message_set_string (message, "output", output);
    pn_node_log_info (node, "%s", output);
    g_free (output);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_zigbee_permit_join_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnZigbeePermitJoin *self = PN_ZIGBEE_PERMIT_JOIN (object);

    switch (prop_id)
    {
    case PROP_MINUTES:
        g_value_set_uint (value, self->minutes);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_zigbee_permit_join_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnZigbeePermitJoin *self = PN_ZIGBEE_PERMIT_JOIN (object);

    switch (prop_id)
    {
    case PROP_MINUTES:
        {
            guint minutes = g_value_get_uint (value);
            if (minutes != self->minutes)
            {
                self->minutes = minutes;
                g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MINUTES]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject / class plumbing                                           */
/* ------------------------------------------------------------------ */

static void
pn_zigbee_permit_join_class_init (PnZigbeePermitJoinClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_zigbee_permit_join_get_property;
    object_class->set_property = pn_zigbee_permit_join_set_property;

    node_class->receive = pn_zigbee_permit_join_receive;

    node_class->palette_icon = PN_ZIGBEE_PERMIT_JOIN_ICON;
    node_class->class_name   = "Zigbee Permit Join";
    node_class->icon         = PN_ZIGBEE_PERMIT_JOIN_ICON;
    node_class->color        = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category     = "Zigbee";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_MINUTES] = g_param_spec_uint (
            "minutes", "Join window (minutes)",
            "How long to hold the Zigbee2MQTT pairing window open, in "
            "minutes.  On every inbound message (treated as a trigger) "
            "the node emits a command on "
            "zigbee2mqtt/bridge/request/permit_join with a JSON "
            "{\"value\":true,\"time\":<seconds>} payload, where seconds "
            "= minutes * 60, so a downstream MQTT Sink opens the join "
            "window straight away.  Defaults to 4 minutes.",
            0, G_MAXUINT, PN_ZIGBEE_PERMIT_JOIN_DEFAULT_MINUTES,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_zigbee_permit_join_init (PnZigbeePermitJoin *self)
{
    PnNode  *node    = PN_NODE (self);
    PnColor  magenta = { 0.78, 0.27, 0.60, 1.0 };

    self->minutes = PN_ZIGBEE_PERMIT_JOIN_DEFAULT_MINUTES;

    /* Seed the *instance* visual state.  The class fields feed the
     * palette and metadata, but the worksheet body paints from the
     * instance icon/color (pn_node_get_icon / pn_node_get_color), which
     * have no class fallback -- so a node that only sets the class fields
     * renders with a blank icon over the neutral grey default (which
     * reads as "disabled").  The minutes field has a sensible default,
     * so there is no unconfigured-error state. */
    pn_node_set_icon       (node, PN_ZIGBEE_PERMIT_JOIN_ICON);
    pn_node_set_color      (node, &magenta);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

PnZigbeePermitJoin *
pn_zigbee_permit_join_new (void)
{
    return g_object_new (PN_TYPE_ZIGBEE_PERMIT_JOIN, NULL);
}
