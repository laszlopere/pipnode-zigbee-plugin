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

#include "pn-zigbee-water-leak.h"

#include <pn-message.h>

#include <json-glib/json-glib.h>
#include <string.h>

/* Z2M's default base topic; the friendly-name tail used as the fallback
 * device label is whatever follows "zigbee2mqtt/".  Pinned to the
 * default for parity with the other nodes in this plugin
 * (#PnZigbeeSource, #PnZigbeeRemote). */
#define PN_ZIGBEE_BASE_TOPIC "zigbee2mqtt"

/* fa-tint U+F043 -- a water drop, the natural glyph for a leak sensor.
 * Magenta body, matching the rest of the Zigbee palette
 * (#PnZigbeeSource / #PnZigbeeRemote). */
#define PN_ZIGBEE_WATER_LEAK_ICON "\xef\x81\x83"

struct _PnZigbeeWaterLeak
{
    PnNode parent_instance;

    /* Optional remote filter.  An empty / NULL value is a wildcard
     * (match any device), so a freshly-dropped node tracks every
     * water-leak sensor it sees.  Set to narrow the node to one
     * specific sensor. */
    gchar *friendly_name;

    /* Which water_leak edges to forward (begin / end / both). */
    PnZigbeeWaterLeakMode mode;

    /* Last water_leak value seen per device, keyed by the resolved
     * device name (friendlyName or topic tail).  Edge detection needs
     * the prior state, and with an empty friendly-name filter the node
     * sees several sensors at once, so the state is tracked per device
     * rather than as a single flag.  Value is GINT_TO_POINTER (0/1);
     * a key's *absence* means "not yet observed". */
    GHashTable *last_state;
};

G_DEFINE_TYPE (PnZigbeeWaterLeak, pn_zigbee_water_leak, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_FRIENDLY_NAME,
    PROP_MODE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Enum type                                                          */
/* ------------------------------------------------------------------ */

GType
pn_zigbee_water_leak_mode_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        /* The value_nick strings are what the host paints into the
         * property dialog's combo box (pn-node-dialog.c). */
        static const GEnumValue values[] = {
            { PN_ZIGBEE_WATER_LEAK_BEGIN, "PN_ZIGBEE_WATER_LEAK_BEGIN",
              "Leak begins" },
            { PN_ZIGBEE_WATER_LEAK_END,   "PN_ZIGBEE_WATER_LEAK_END",
              "Leak ends" },
            { PN_ZIGBEE_WATER_LEAK_BOTH,  "PN_ZIGBEE_WATER_LEAK_BOTH",
              "Both" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnZigbeeWaterLeakMode", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Payload introspection                                              */
/* ------------------------------------------------------------------ */

/** Borrow @message's `data.payload` as a #JsonObject, or %NULL when the
 *  payload is absent or not an object (Z2M occasionally publishes bare
 *  strings -- availability, etc. -- which carry no water_leak state). */
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

/** Read a boolean member @key off @obj into @out.  Returns %TRUE only
 *  when the member is present and a JSON boolean; %FALSE (leaving @out
 *  untouched) otherwise, which is how a non-leak-sensor publish -- one
 *  with no water_leak member at all -- is told apart from a sensor
 *  reporting water_leak=false. */
static gboolean
peek_boolean (
        JsonObject  *obj,
        const gchar *key,
        gboolean    *out)
{
    JsonNode *n;

    if (obj == NULL || !json_object_has_member (obj, key))
        return FALSE;
    n = json_object_get_member (obj, key);
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
        return FALSE;
    if (json_node_get_value_type (n) != G_TYPE_BOOLEAN)
        return FALSE;
    *out = json_node_get_boolean (n);
    return TRUE;
}

/** Best human-readable name for the sensor behind @message, also used as
 *  the per-device key for edge tracking.
 *
 *  Preference order:
 *    1. `data.payload.device.friendlyName` -- the identity block a
 *       #PnZigbeeSource injects (its default), the actual Z2M friendly
 *       name a user recognises.
 *    2. The friendly-name tail of the envelope topic
 *       (`zigbee2mqtt/<tail>` -> `<tail>`); for an unrenamed device this
 *       is the IEEE address (e.g. 0xa4c13860befb28ec).
 *    3. The bare topic, then "unknown", as last resorts.
 *
 *  Returns a borrowed pointer valid for the duration of the call (it
 *  points either into @message's JSON tree or into its topic string). */
static const gchar *
resolve_sensor_name (
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

/** Does @message's sensor match the configured friendly-name filter?
 *  An empty filter is a wildcard.  Otherwise it matches when the
 *  configured name equals either the injected
 *  `data.payload.device.friendlyName` or the friendly-name tail of the
 *  envelope topic, so the filter works whether or not an upstream
 *  #PnZigbeeSource injected the device block. */
static gboolean
friendly_name_matches (
        PnZigbeeWaterLeak *self,
        PnMessage         *message,
        JsonObject        *payload)
{
    const gchar *topic;

    if (self->friendly_name == NULL || *self->friendly_name == '\0')
        return TRUE;

    if (payload != NULL && json_object_has_member (payload, "device"))
    {
        JsonNode *device_node = json_object_get_member (payload, "device");
        if (device_node != NULL && JSON_NODE_HOLDS_OBJECT (device_node))
        {
            const gchar *fname = peek_nonempty_string (
                    json_node_get_object (device_node), "friendlyName");
            if (fname != NULL && g_strcmp0 (fname, self->friendly_name) == 0)
                return TRUE;
        }
    }

    topic = pn_message_get_topic (message);
    if (topic != NULL && g_str_has_prefix (topic, PN_ZIGBEE_BASE_TOPIC "/"))
    {
        const gchar *tail = topic + strlen (PN_ZIGBEE_BASE_TOPIC "/");
        if (g_strcmp0 (tail, self->friendly_name) == 0)
            return TRUE;
    }

    return FALSE;
}

/** Does this node forward an edge that ends in @leaking, given its
 *  configured mode?  A true value is the leak-begin edge, false the
 *  leak-end edge. */
static gboolean
mode_passes (
        PnZigbeeWaterLeak *self,
        gboolean           leaking)
{
    switch (self->mode)
    {
    case PN_ZIGBEE_WATER_LEAK_BEGIN: return leaking;
    case PN_ZIGBEE_WATER_LEAK_END:   return !leaking;
    case PN_ZIGBEE_WATER_LEAK_BOTH:  return TRUE;
    default:                         return TRUE;
    }
}

/* ------------------------------------------------------------------ */
/*  Receive -- edge-filter, reshape, forward                           */
/* ------------------------------------------------------------------ */

/** Complete override of the base receive vfunc.  Tracks the last
 *  water_leak value per device and only forwards on a transition that
 *  the `mode` selects; survivors are stamped into the canonical pipnode
 *  contract in place and emitted.  Mutating in place is safe: the wire
 *  layer hands each fan-out branch its own #PnMessage clone, so this
 *  node owns the copy it receives.
 *
 *  The first publish seen from a device only seeds its state (no emit):
 *  with no prior value there is no transition to report, and seeding
 *  silently avoids firing a spurious "begin" just because the node was
 *  dropped (or the worksheet reloaded) while a leak happened to be
 *  active.  The next genuine change then fires. */
static void
pn_zigbee_water_leak_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnZigbeeWaterLeak *self = PN_ZIGBEE_WATER_LEAK (node);
    JsonObject        *payload;
    const gchar       *name;
    gboolean           leaking;
    gboolean           had_prev;
    gboolean           prev;
    gpointer           stored;
    gchar             *output;

    payload = get_payload_object (message);
    if (payload == NULL)
        return;

    /* A water-leak sensor carries a boolean `water_leak`.  Publishes
     * without it (other device types, bridge traffic, bare-string
     * availability) are silently dropped -- this is the routine
     * broker-traffic read path, and logging it would flood the per-node
     * log ring (see TODO channel-3 rule). */
    if (!peek_boolean (payload, "water_leak", &leaking))
        return;

    /* Optional device filter: empty friendly-name is a wildcard. */
    if (!friendly_name_matches (self, message, payload))
        return;

    name = resolve_sensor_name (message, payload);

    /* Edge detection against the per-device last value.  A first
     * observation seeds the state and forwards nothing. */
    had_prev = g_hash_table_lookup_extended (self->last_state, name,
                                             NULL, &stored);
    prev     = had_prev && (GPOINTER_TO_INT (stored) != 0);

    g_hash_table_insert (self->last_state, g_strdup (name),
                         GINT_TO_POINTER (leaking ? 1 : 0));

    if (!had_prev)
        return;                         /* seed only */
    if (prev == leaking)
        return;                         /* no transition */
    if (!mode_passes (self, leaking))
        return;                         /* edge not selected by mode */

    /* Reshape into the canonical pipnode message contract so a
     * downstream LED / Debug / Graph / Notify reads the same
     * value/success/output shape it gets from any other node.  The
     * original envelope and `data.payload` (battery, linkquality,
     * tamper, voltage, the injected device block) are left intact. */
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_double  (message, "value",   leaking ? 1.0 : 0.0);
    pn_message_set_string  (message, "device",  name);

    output = g_strdup_printf ("Water leak %s on %s",
                              leaking ? "detected" : "cleared", name);
    pn_message_set_string (message, "output", output);

    pn_node_log_info (node, "%s", output);
    g_free (output);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

/** Replace the friendly-name filter with a copy of @value, normalising
 *  NULL / "" to NULL so both forms behave as the same wildcard, and
 *  notify on a real change. */
static void
set_friendly_name (
        PnZigbeeWaterLeak *self,
        const gchar       *value)
{
    gchar *replacement = (value != NULL && *value != '\0')
                       ? g_strdup (value) : NULL;

    if (g_strcmp0 (self->friendly_name, replacement) == 0)
    {
        g_free (replacement);
        return;
    }

    g_free (self->friendly_name);
    self->friendly_name = replacement;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_FRIENDLY_NAME]);
}

static void
pn_zigbee_water_leak_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnZigbeeWaterLeak *self = PN_ZIGBEE_WATER_LEAK (object);

    switch (prop_id)
    {
    case PROP_FRIENDLY_NAME:
        g_value_set_string (value, self->friendly_name);
        break;
    case PROP_MODE:
        g_value_set_enum (value, self->mode);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_zigbee_water_leak_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnZigbeeWaterLeak *self = PN_ZIGBEE_WATER_LEAK (object);

    switch (prop_id)
    {
    case PROP_FRIENDLY_NAME:
        set_friendly_name (self, g_value_get_string (value));
        break;
    case PROP_MODE:
        {
            PnZigbeeWaterLeakMode mode = g_value_get_enum (value);
            if (mode != self->mode)
            {
                self->mode = mode;
                g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MODE]);
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
pn_zigbee_water_leak_finalize (GObject *object)
{
    PnZigbeeWaterLeak *self = PN_ZIGBEE_WATER_LEAK (object);

    g_clear_pointer (&self->friendly_name, g_free);
    g_clear_pointer (&self->last_state, g_hash_table_destroy);

    G_OBJECT_CLASS (pn_zigbee_water_leak_parent_class)->finalize (object);
}

static void
pn_zigbee_water_leak_class_init (PnZigbeeWaterLeakClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_zigbee_water_leak_get_property;
    object_class->set_property = pn_zigbee_water_leak_set_property;
    object_class->finalize     = pn_zigbee_water_leak_finalize;

    node_class->receive = pn_zigbee_water_leak_receive;

    node_class->palette_icon = PN_ZIGBEE_WATER_LEAK_ICON;
    node_class->class_name   = "Zigbee Water Leak";
    node_class->icon         = PN_ZIGBEE_WATER_LEAK_ICON;
    node_class->color        = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category     = "Zigbee";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_FRIENDLY_NAME] = g_param_spec_string (
            "friendly-name", "Friendly name",
            "Optional sensor filter.  When set, only water_leak edges "
            "from this sensor pass; the value is matched against the "
            "injected data.payload.device.friendlyName (when an upstream "
            "Zigbee Source injects device info) or, failing that, the "
            "friendly-name tail of the envelope topic "
            "(zigbee2mqtt/<tail>, the bare device id for an unrenamed "
            "device).  Empty (default) tracks every water-leak sensor.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MODE] = g_param_spec_enum (
            "mode", "Mode",
            "Which water_leak transitions to forward.  \"Leak begins\" "
            "passes only the false->true edge (a leak just started), "
            "\"Leak ends\" only the true->false edge (a leak just "
            "cleared), and \"Both\" (the default) passes either.  The "
            "node tracks the last value per device and drops the "
            "repeated same-state updates Z2M republishes on every "
            "battery / linkquality report.",
            PN_TYPE_ZIGBEE_WATER_LEAK_MODE, PN_ZIGBEE_WATER_LEAK_BOTH,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_zigbee_water_leak_init (PnZigbeeWaterLeak *self)
{
    PnNode  *node    = PN_NODE (self);
    PnColor  magenta = { 0.78, 0.27, 0.60, 1.0 };

    self->mode       = PN_ZIGBEE_WATER_LEAK_BOTH;
    self->last_state = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, NULL);

    /* Seed the *instance* visual state.  The class fields feed the
     * palette and metadata, but the worksheet body paints from the
     * instance icon/color (pn_node_get_icon / pn_node_get_color), which
     * have no class fallback -- so a node that only sets the class
     * fields renders with a blank icon over the neutral grey default
     * (which reads as "disabled").  friendly-name is an optional filter,
     * not mandatory config, so there is no unconfigured-error state. */
    pn_node_set_icon       (node, PN_ZIGBEE_WATER_LEAK_ICON);
    pn_node_set_color      (node, &magenta);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

PnZigbeeWaterLeak *
pn_zigbee_water_leak_new (void)
{
    return g_object_new (PN_TYPE_ZIGBEE_WATER_LEAK, NULL);
}
