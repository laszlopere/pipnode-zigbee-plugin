/*
 * Copyright (C) 2026 Laszlo Pere
 *
 * This file is part of pipnode-zigbee-plugin, a plugin for Pipnode, and
 * is free software under the GNU General Public License version 3 or (at
 * your option) any later version.  See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit tests for PnZigbeeSink: an appearance-only subclass of PnMqttSink
 * that sits with the rest of the Zigbee palette.  It overrides none of
 * the base's vfuncs, so there is no publish behaviour to exercise here --
 * the test pins the Zigbee identity (class name, category, icon, ports),
 * guarding the class-level class_name fallback against a regression to a
 * shadowing per-instance label (see TODO (1)).
 */

#include "pn-test.h"
#include "pn-zigbee-sink.h"

#define SINK_ICON "\xef\x87\x98"   /* U+F1D8, the paper-plane glyph */

static void
test_metadata (void)
{
    PnNode *node = PN_NODE (pn_zigbee_sink_new ());

    /* class_name must resolve to the subclass label "Zigbee Sink", not the
     * base "MQTT Sink": the base no longer seeds a per-instance label and
     * this node does not re-stamp one, so the class-level pin shows through
     * pn_node_get_class_name()'s fallback. */
    CHECK_STR_EQ (pn_node_get_class_name (node), "Zigbee Sink");
    CHECK_STR_EQ (pn_node_get_category (node),   "Zigbee");
    CHECK_STR_EQ (pn_node_get_icon (node),       SINK_ICON);

    /* A sink: input only, no output. */
    CHECK (pn_node_get_has_input (node));
    CHECK_FALSE (pn_node_get_has_output (node));

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-sink");
    t_add ("metadata", test_metadata);
    return t_run ();
}
