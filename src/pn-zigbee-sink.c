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

#include "pn-zigbee-sink.h"

/* fa-paper-plane U+F1D8 -- the same "publish / send out" glyph the base
 * #PnMqttSink wears, since this is the same role (an MQTT publisher) just
 * preconfigured to sit with the Zigbee palette.  Worn in the Zigbee
 * magenta body that the rest of the plugin uses (#PnZigbeeSource /
 * #PnZigbeeRelayCommand), which is the only thing this subclass changes
 * for now.  No "needs configuration" warning glyph: like #PnZigbeeSource
 * a freshly-dropped node is already usable (it follows the primary broker
 * and publishes each message to its envelope topic), so the red-❗ cue the
 * mandatory-field nodes use would be misleading. */
#define PN_ZIGBEE_SINK_ICON "\xef\x87\x98"

struct _PnZigbeeSink
{
    PnMqttSink parent_instance;
};

G_DEFINE_TYPE (PnZigbeeSink, pn_zigbee_sink, PN_TYPE_MQTT_SINK)

static void
pn_zigbee_sink_class_init (PnZigbeeSinkClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    /* Appearance only -- the base's vfuncs (process_message / report_error)
     * are left at their defaults, so the node behaves exactly like a plain
     * #PnMqttSink while looking like the rest of the Zigbee palette. */
    node_class->palette_icon = PN_ZIGBEE_SINK_ICON;
    node_class->class_name   = "Zigbee Sink";
    node_class->icon         = PN_ZIGBEE_SINK_ICON;
    node_class->color        = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category     = "Zigbee";
}

static void
pn_zigbee_sink_init (PnZigbeeSink *self)
{
    PnColor magenta = { 0.78, 0.27, 0.60, 1.0 };

    /* Seed per-instance visual state so the worksheet body paints the
     * Zigbee identity (magenta + paper-plane glyph) immediately, the same
     * way #PnZigbeeSource does -- the class fields only feed the palette
     * entry, the body reads the instance. */
    pn_node_set_icon  (PN_NODE (self), PN_ZIGBEE_SINK_ICON);
    pn_node_set_color (PN_NODE (self), &magenta);

    /* class_name is pinned on PnNodeClass.class_name in class_init and
     * resolved through pn_node_get_class_name()'s class-level fallback, so
     * the node shows "Zigbee Sink" without a per-instance re-stamp.  The
     * base #PnMqttSink no longer seeds a per-instance "MQTT Sink" either
     * (pipnode 5b568f6), so there is nothing left to shadow it. */
}

PnZigbeeSink *
pn_zigbee_sink_new (void)
{
    return g_object_new (PN_TYPE_ZIGBEE_SINK, NULL);
}
