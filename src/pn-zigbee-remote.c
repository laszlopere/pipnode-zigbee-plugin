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

#include "pn-zigbee-remote.h"

#include <pn-message.h>

#include <json-glib/json-glib.h>
#include <string.h>

/* Z2M's default base topic; the friendly-name tail used as the
 * fallback remote label is whatever follows "zigbee2mqtt/".  Pinned to
 * the default for parity with the other nodes in this plugin
 * (#PnZigbeeSource, #PnZigbeeSwitch). */
#define PN_ZIGBEE_BASE_TOPIC "zigbee2mqtt"

/* fa-hand-pointer U+F25A -- a finger pressing, the natural glyph for a
 * remote button event.  Magenta body, matching the rest of the Zigbee
 * palette (#PnZigbeeSource / #PnZigbeeSwitch). */
#define PN_ZIGBEE_REMOTE_ICON "\xef\x89\x9a"

struct _PnZigbeeRemote
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnZigbeeRemote, pn_zigbee_remote, PN_TYPE_NODE)

/* ------------------------------------------------------------------ */
/*  Payload introspection                                              */
/* ------------------------------------------------------------------ */

/** Borrow @message's `data.payload` as a #JsonObject, or %NULL when the
 *  payload is absent or not an object (Z2M occasionally publishes bare
 *  strings -- availability, etc. -- which are never remote events). */
static JsonObject *
get_payload_object (PnMessage *message)
{
    JsonNode *payload_node;

    payload_node = pn_message_get_member (message, "payload");
    if (payload_node == NULL || !JSON_NODE_HOLDS_OBJECT (payload_node))
        return NULL;
    return json_node_get_object (payload_node);
}

/** Read a non-empty string member @key off @obj.  Returns a borrowed
 *  pointer (valid while @obj is alive) or %NULL when the member is
 *  missing, not a string, or the empty string. */
static const gchar *
peek_nonempty_string (
        JsonObject  *obj,
        const gchar *key)
{
    JsonNode    *n;
    const gchar *s;

    if (obj == NULL || !json_object_has_member (obj, key))
        return NULL;
    n = json_object_get_member (obj, key);
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
        return NULL;
    if (json_node_get_value_type (n) != G_TYPE_STRING)
        return NULL;
    s = json_node_get_string (n);
    if (s == NULL || *s == '\0')
        return NULL;
    return s;
}

/** Best human-readable name for the remote behind @message.
 *
 *  Preference order:
 *    1. `data.payload.device.friendlyName` -- the identity block a
 *       #PnZigbeeSource injects (its default), which is the actual Z2M
 *       friendly name a user recognises.
 *    2. The friendly-name tail of the envelope topic
 *       (`zigbee2mqtt/<tail>` -> `<tail>`), which for an unrenamed
 *       device is the IEEE address (e.g. 0xa4c138c974b825c2).
 *    3. The bare topic, then "unknown", as last resorts.
 *
 *  Returns a borrowed pointer valid for the duration of the call (it
 *  points either into @message's JSON tree or into its topic string). */
static const gchar *
resolve_remote_name (
        PnMessage  *message,
        JsonObject *payload)
{
    const gchar *topic;
    JsonNode    *device_node;

    if (payload != NULL && json_object_has_member (payload, "device"))
    {
        device_node = json_object_get_member (payload, "device");
        if (device_node != NULL && JSON_NODE_HOLDS_OBJECT (device_node))
        {
            const gchar *fname = peek_nonempty_string (
                    json_node_get_object (device_node), "friendlyName");
            if (fname != NULL)
                return fname;
        }
    }

    topic = pn_message_get_topic (message);
    if (topic != NULL && *topic != '\0')
    {
        if (g_str_has_prefix (topic, PN_ZIGBEE_BASE_TOPIC "/"))
        {
            const gchar *tail = topic + strlen (PN_ZIGBEE_BASE_TOPIC "/");
            if (*tail != '\0')
                return tail;
        }
        return topic;
    }

    return "unknown";
}

/* ------------------------------------------------------------------ */
/*  Receive -- filter, reshape, forward                                */
/* ------------------------------------------------------------------ */

/** Complete override of the base receive vfunc.  Drops every message
 *  that is not a remote button press; for the survivors it stamps the
 *  canonical pipnode contract onto the message in place and forwards
 *  it.  Mutating in place is safe: the wire layer hands each fan-out
 *  branch its own #PnMessage clone, so this node owns the copy it
 *  receives. */
static void
pn_zigbee_remote_receive (
        PnNode    *node,
        PnMessage *message)
{
    JsonObject  *payload;
    const gchar *action;
    const gchar *name;
    gchar       *output;

    payload = get_payload_object (message);
    if (payload == NULL)
        return;

    /* A real button press carries a non-empty string `action`.  The
     * idle / state-only publishes Z2M interleaves (and the "released"
     * empty-action frames some converters emit) have none, so they
     * are filtered out here. */
    action = peek_nonempty_string (payload, "action");
    if (action == NULL)
        return;

    name = resolve_remote_name (message, payload);

    /* Reshape into the canonical pipnode message contract so a
     * downstream LED / Debug / Graph reads the same value/success/output
     * shape it gets from any other node.  The original envelope and
     * `data.payload` (action, linkquality, injected device block) are
     * left intact for nodes that want the detail. */
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_double  (message, "value",   1.0);

    output = g_strdup_printf ("Remote %s activated (action: %s)",
                              name, action);
    pn_message_set_string (message, "output", output);

    pn_node_log_info (node, "%s", output);
    g_free (output);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  GObject / class plumbing                                           */
/* ------------------------------------------------------------------ */

static void
pn_zigbee_remote_class_init (PnZigbeeRemoteClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive = pn_zigbee_remote_receive;

    node_class->palette_icon = PN_ZIGBEE_REMOTE_ICON;
    node_class->class_name   = "Zigbee Remote";
    node_class->icon         = PN_ZIGBEE_REMOTE_ICON;
    node_class->color        = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category     = "Zigbee";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;
}

static void
pn_zigbee_remote_init (PnZigbeeRemote *self)
{
    PnNode  *node    = PN_NODE (self);
    PnColor  magenta = { 0.78, 0.27, 0.60, 1.0 };

    /* Seed the *instance* visual state.  The class fields above feed
     * the palette and metadata, but the worksheet body paints from the
     * instance icon/color (pn_node_get_icon / pn_node_get_color), which
     * have no class fallback -- so a node that only sets the class
     * fields renders with a blank icon over the neutral grey default
     * (the latter reads as "disabled").  class_name is NOT seeded here:
     * pn_node_get_class_name() already falls back to the class field
     * (pinned in class_init), and pn_node_set_class_name() is
     * deprecated for instance init use. */
    pn_node_set_icon       (node, PN_ZIGBEE_REMOTE_ICON);
    pn_node_set_color      (node, &magenta);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

PnZigbeeRemote *
pn_zigbee_remote_new (void)
{
    return g_object_new (PN_TYPE_ZIGBEE_REMOTE, NULL);
}
