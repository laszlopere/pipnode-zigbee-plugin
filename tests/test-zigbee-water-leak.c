/*
 * Copyright (C) 2026 Laszlo Pere
 *
 * This file is part of pipnode-zigbee-plugin, a plugin for Pipnode, and
 * is free software under the GNU General Public License version 3 or (at
 * your option) any later version.  See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit tests for PnZigbeeWaterLeak: edge-filters a Z2M leak sensor's
 * boolean `water_leak` (begin / end / both) per device and reshapes the
 * surviving transitions into the canonical message shape.
 */

#include "pn-test.h"
#include "pn-zigbee-water-leak.h"

#include <json-glib/json-glib.h>

/* Build a leak-sensor publish.  @leak is 1 (water_leak=true), 0
 * (water_leak=false) or -1 (omit the member entirely -- a non-sensor
 * publish).  @device_fname, when non-NULL, injects a device.friendlyName
 * block as a #PnZigbeeSource would add. */
static PnMessage *
leak (const char *topic, int leak, const char *device_fname)
{
    PnMessage  *msg     = pn_message_new (NULL, topic);
    JsonObject *payload = json_object_new ();
    JsonNode   *node    = json_node_new (JSON_NODE_OBJECT);

    if (leak >= 0)
        json_object_set_boolean_member (payload, "water_leak", leak != 0);

    if (device_fname != NULL)
    {
        JsonObject *device = json_object_new ();
        json_object_set_string_member (device, "friendlyName", device_fname);
        json_object_set_object_member (payload, "device", device);
    }

    json_node_take_object (node, payload);
    pn_message_set_member (msg, "payload", node);
    return msg;
}

/* Feed one publish and unref it (the node owns its received clone). */
static void
feed (PnNode *node, PnMessage *msg)
{
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_first_seen_seeds_only (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_water_leak_new ());
    TCapture cap;

    /* The first publish from a device only seeds state -- no emit, even
     * though it carries a leak. */
    t_capture_attach (node, &cap);
    feed (node, leak ("zigbee2mqtt/s", 1, NULL));
    CHECK_INT_EQ (cap.count, 0);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_both_forwards_each_edge (void)
{
    PnNode    *node = PN_NODE (pn_zigbee_water_leak_new ());
    TCapture   cap;
    JsonObject *data;

    /* Default mode is Both. */
    t_capture_attach (node, &cap);

    feed (node, leak ("zigbee2mqtt/s", 0, NULL));   /* seed: dry */
    CHECK_INT_EQ (cap.count, 0);

    feed (node, leak ("zigbee2mqtt/s", 1, NULL));   /* begin edge */
    CHECK_INT_EQ (cap.count, 1);
    data = pn_message_get_data (cap.last);
    CHECK (json_object_get_boolean_member (data, "success"));
    CHECK_NEAR (json_object_get_double_member (data, "value"), 1.0);
    CHECK_STR_EQ (json_object_get_string_member (data, "output"),
                  "Water leak detected on s");
    CHECK_STR_EQ (json_object_get_string_member (data, "device"), "s");

    feed (node, leak ("zigbee2mqtt/s", 0, NULL));   /* end edge */
    CHECK_INT_EQ (cap.count, 2);
    data = pn_message_get_data (cap.last);
    CHECK_NEAR (json_object_get_double_member (data, "value"), 0.0);
    CHECK_STR_EQ (json_object_get_string_member (data, "output"),
                  "Water leak cleared on s");

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_repeated_state_dropped (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_water_leak_new ());
    TCapture cap;

    t_capture_attach (node, &cap);

    feed (node, leak ("zigbee2mqtt/s", 0, NULL));   /* seed */
    feed (node, leak ("zigbee2mqtt/s", 1, NULL));   /* begin -> emit */
    CHECK_INT_EQ (cap.count, 1);

    /* Two more "still leaking" reports (battery/linkquality refreshes)
     * repeat the same state and are dropped. */
    feed (node, leak ("zigbee2mqtt/s", 1, NULL));
    feed (node, leak ("zigbee2mqtt/s", 1, NULL));
    CHECK_INT_EQ (cap.count, 1);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_mode_begin_only (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_water_leak_new ());
    TCapture cap;

    g_object_set (node, "mode", PN_ZIGBEE_WATER_LEAK_BEGIN, NULL);
    t_capture_attach (node, &cap);

    feed (node, leak ("zigbee2mqtt/s", 0, NULL));   /* seed */
    feed (node, leak ("zigbee2mqtt/s", 1, NULL));   /* begin -> emit */
    CHECK_INT_EQ (cap.count, 1);
    feed (node, leak ("zigbee2mqtt/s", 0, NULL));   /* end  -> dropped */
    CHECK_INT_EQ (cap.count, 1);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_mode_end_only (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_water_leak_new ());
    TCapture cap;

    g_object_set (node, "mode", PN_ZIGBEE_WATER_LEAK_END, NULL);
    t_capture_attach (node, &cap);

    feed (node, leak ("zigbee2mqtt/s", 0, NULL));   /* seed */
    feed (node, leak ("zigbee2mqtt/s", 1, NULL));   /* begin -> dropped */
    CHECK_INT_EQ (cap.count, 0);
    feed (node, leak ("zigbee2mqtt/s", 0, NULL));   /* end  -> emit */
    CHECK_INT_EQ (cap.count, 1);
    CHECK_NEAR (json_object_get_double_member (pn_message_get_data (cap.last),
                                               "value"), 0.0);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_no_water_leak_member_dropped (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_water_leak_new ());
    TCapture cap;
    PnMessage *no_payload;

    t_capture_attach (node, &cap);

    /* A payload with no water_leak member is not a leak sensor. */
    feed (node, leak ("zigbee2mqtt/s", -1, NULL));
    CHECK_INT_EQ (cap.count, 0);

    /* A message with no payload object at all is dropped too. */
    no_payload = pn_message_new (NULL, "zigbee2mqtt/s");
    feed (node, no_payload);
    CHECK_INT_EQ (cap.count, 0);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_friendly_name_filter (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_water_leak_new ());
    TCapture cap;

    g_object_set (node, "friendly-name", "kitchen", NULL);
    t_capture_attach (node, &cap);

    /* A different sensor is dropped at the filter -- and, importantly,
     * does not seed state for the configured one. */
    feed (node, leak ("zigbee2mqtt/bathroom", 0, NULL));
    feed (node, leak ("zigbee2mqtt/bathroom", 1, NULL));
    CHECK_INT_EQ (cap.count, 0);

    /* The configured sensor: seed then a begin edge passes. */
    feed (node, leak ("zigbee2mqtt/kitchen", 0, NULL));
    feed (node, leak ("zigbee2mqtt/kitchen", 1, NULL));
    CHECK_INT_EQ (cap.count, 1);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_per_device_tracking (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_water_leak_new ());
    TCapture cap;

    /* Wildcard (no friendly-name): two sensors are tracked
     * independently, so one's seed does not satisfy the other's. */
    t_capture_attach (node, &cap);

    feed (node, leak ("zigbee2mqtt/a", 0, NULL));   /* seed a */
    feed (node, leak ("zigbee2mqtt/b", 0, NULL));   /* seed b */
    CHECK_INT_EQ (cap.count, 0);

    feed (node, leak ("zigbee2mqtt/a", 1, NULL));   /* a begin */
    CHECK_INT_EQ (cap.count, 1);
    CHECK_STR_EQ (json_object_get_string_member (pn_message_get_data (cap.last),
                                                 "device"), "a");

    feed (node, leak ("zigbee2mqtt/b", 1, NULL));   /* b begin */
    CHECK_INT_EQ (cap.count, 2);
    CHECK_STR_EQ (json_object_get_string_member (pn_message_get_data (cap.last),
                                                 "device"), "b");

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_device_block_name (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_water_leak_new ());
    TCapture cap;

    /* The injected device.friendlyName is preferred over the topic tail
     * for both the output name and the per-device tracking key. */
    t_capture_attach (node, &cap);

    feed (node, leak ("zigbee2mqtt/0xabc", 0, "Kitchen"));
    feed (node, leak ("zigbee2mqtt/0xabc", 1, "Kitchen"));
    CHECK_INT_EQ (cap.count, 1);
    CHECK_STR_EQ (json_object_get_string_member (pn_message_get_data (cap.last),
                                                 "output"),
                  "Water leak detected on Kitchen");

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_metadata (void)
{
    PnNode *node = PN_NODE (pn_zigbee_water_leak_new ());
    PnZigbeeWaterLeakMode mode;

    CHECK (pn_node_get_has_input (node));
    CHECK (pn_node_get_has_output (node));
    CHECK_STR_EQ (pn_node_get_class_name (node), "Zigbee Water Leak");
    CHECK_STR_EQ (pn_node_get_category (node), "Zigbee");

    /* Default mode is Both. */
    g_object_get (node, "mode", &mode, NULL);
    CHECK_INT_EQ (mode, PN_ZIGBEE_WATER_LEAK_BOTH);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-water-leak");
    t_add ("first_seen_seeds_only",     test_first_seen_seeds_only);
    t_add ("both_forwards_each_edge",   test_both_forwards_each_edge);
    t_add ("repeated_state_dropped",    test_repeated_state_dropped);
    t_add ("mode_begin_only",           test_mode_begin_only);
    t_add ("mode_end_only",             test_mode_end_only);
    t_add ("no_water_leak_dropped",     test_no_water_leak_member_dropped);
    t_add ("friendly_name_filter",      test_friendly_name_filter);
    t_add ("per_device_tracking",       test_per_device_tracking);
    t_add ("device_block_name",         test_device_block_name);
    t_add ("metadata",                  test_metadata);
    return t_run ();
}
