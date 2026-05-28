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

#include "pn-zigbee-source.h"

#include <string.h>

/* fa-rss U+F09E -- same glyph the base #PnMqtt uses, since this is the
 * same role (an MQTT subscriber) just preconfigured for the Z2M topic
 * tree.  No "needs configuration" warning glyph here: a freshly-dropped
 * node already has a usable subscribe filter (`zigbee2mqtt/#`), so the
 * red-❗ "fill me in" cue PnZigbeeSwitch uses would be misleading. */
#define PN_ZIGBEE_SOURCE_ICON "\xef\x82\x9e"

/* The single Z2M topic the filter targets.  Kept as a string literal
 * rather than a property because the topic name is fixed by Z2M's
 * `bridge/logging` convention; users who want different exclusions can
 * narrow the subscribe-filter directly. */
#define PN_ZIGBEE_LOGGING_TOPIC "zigbee2mqtt/bridge/logging"

struct _PnZigbeeSource
{
    PnMqtt parent_instance;

    /* Whether accept_topic should drop `zigbee2mqtt/bridge/logging` on
     * the network thread.  Cross-thread access (set on the main
     * thread, read on libmosquitto's network thread) is handled
     * through g_atomic_int_get/set so the read is portable and the
     * write is publishing-safe -- the property dialog may flip this
     * at any time while the broker connection is live. */
    gint filter_logging;
};

G_DEFINE_TYPE (PnZigbeeSource, pn_zigbee_source, PN_TYPE_MQTT)

enum {
    PROP_0,
    PROP_FILTER_LOGGING,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Early topic filter                                                 */
/* ------------------------------------------------------------------ */

/** Override of #PnMqttClass::accept_topic.  Runs on libmosquitto's
 *  network thread for every inbound PUBLISH, BEFORE the base builds a
 *  #PnMessage -- so a rejected publish costs nothing more than this
 *  string compare.  The only topic dropped here is
 *  `zigbee2mqtt/bridge/logging` (Z2M's log-mirror channel), and only
 *  when #PnZigbeeSource:filter-logging is enabled.  Everything else
 *  delegates to the parent default (which currently accepts
 *  unconditionally) -- chaining instead of returning TRUE outright
 *  stays future-proof against the base default gaining real
 *  behaviour later. */
static gboolean
pn_zigbee_source_accept_topic (
        PnMqtt      *base,
        const gchar *topic)
{
    PnZigbeeSource *self = PN_ZIGBEE_SOURCE (base);

    /* g_atomic_int_get for the read because the field is mutated on
     * the main thread (property dialog) and read here on the network
     * thread.  Cheap on x86/ARM64 (single aligned word load) but the
     * atomic call documents the cross-thread contract and stays
     * correct on weaker memory models. */
    if (g_atomic_int_get (&self->filter_logging) &&
        topic != NULL &&
        strcmp (topic, PN_ZIGBEE_LOGGING_TOPIC) == 0)
        return FALSE;

    return pn_mqtt_real_accept_topic (base, topic);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_zigbee_source_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnZigbeeSource *self = PN_ZIGBEE_SOURCE (object);

    switch (prop_id)
    {
    case PROP_FILTER_LOGGING:
        g_value_set_boolean (value,
                g_atomic_int_get (&self->filter_logging) != 0);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_zigbee_source_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnZigbeeSource *self = PN_ZIGBEE_SOURCE (object);

    switch (prop_id)
    {
    case PROP_FILTER_LOGGING:
    {
        gint want = g_value_get_boolean (value) ? 1 : 0;
        if (g_atomic_int_get (&self->filter_logging) != want)
        {
            g_atomic_int_set (&self->filter_logging, want);
            g_object_notify_by_pspec (object, props[PROP_FILTER_LOGGING]);
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
pn_zigbee_source_class_init (PnZigbeeSourceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);
    PnMqttClass  *mqtt_class   = PN_MQTT_CLASS (klass);

    object_class->get_property = pn_zigbee_source_get_property;
    object_class->set_property = pn_zigbee_source_set_property;

    /* Override only the early filter.  process_message stays at the
     * base default (accept-as-is) because nothing about the inbound
     * shape needs reshaping for the Zigbee category -- downstream
     * #PnZigbeeSwitch / a Debug node both want the message in the
     * exact form the base produced. */
    mqtt_class->accept_topic = pn_zigbee_source_accept_topic;

    node_class->palette_icon = PN_ZIGBEE_SOURCE_ICON;
    node_class->class_name   = "Zigbee Source";
    node_class->icon         = PN_ZIGBEE_SOURCE_ICON;
    node_class->color        = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category     = "Zigbee";

    props[PROP_FILTER_LOGGING] = g_param_spec_boolean (
            "filter-logging", "Filter logging",
            "When TRUE (default), publishes on "
            "zigbee2mqtt/bridge/logging -- Zigbee2MQTT's MQTT mirror "
            "of its own log lines, the channel the Z2M web UI's Logs "
            "tab consumes -- are dropped on the network thread before "
            "a PnMessage is built, so the rest of the flow only sees "
            "device traffic and structured bridge events.  Set FALSE "
            "to forward those log envelopes too (useful when piping "
            "Z2M logs into a Debug node or an external aggregator).  "
            "Has no effect on the other bridge/* topics (event, "
            "state, devices, groups, definitions, response/...): "
            "those carry structured data and always pass through.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_zigbee_source_init (PnZigbeeSource *self)
{
    self->filter_logging = 1;

    /* Default the inherited subscribe-filter to the Z2M topic root so
     * a freshly-dropped node starts pointed at the right tree.  The
     * worksheet loader applies stored properties AFTER instance init,
     * so a saved value in the file overrides this default the same
     * way it would any other property.  Setting via g_object_set
     * (rather than poking a private field) goes through the base
     * class's set_property, which is the supported path -- the
     * property is documented public on #PnMqtt. */
    g_object_set (self, "subscribe-topic", "zigbee2mqtt/#", NULL);
}

PnZigbeeSource *
pn_zigbee_source_new (void)
{
    return g_object_new (PN_TYPE_ZIGBEE_SOURCE, NULL);
}
