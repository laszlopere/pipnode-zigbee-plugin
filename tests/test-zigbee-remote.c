/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * Unit tests for PnZigbeeRemote: filters Z2M button-press events
 * (payload `action`) by optional friendly-name / action and reshapes
 * survivors into the canonical message shape.  Pure transformation.
 */

#include "pn-test.h"
#include "pn-zigbee-remote.h"

#include <json-glib/json-glib.h>

/* Build a publish whose data.payload carries the given `action` (NULL
 * to omit it) and, optionally, an injected device.friendlyName block as
 * a #PnZigbeeSource would add. */
static PnMessage *
press (const char *topic, const char *action, const char *device_fname)
{
    PnMessage  *msg     = pn_message_new (NULL, topic);
    JsonObject *payload = json_object_new ();
    JsonNode   *node    = json_node_new (JSON_NODE_OBJECT);

    if (action != NULL)
        json_object_set_string_member (payload, "action", action);

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

static void
test_forwards_press_wildcard (void)
{
    PnNode    *node = PN_NODE (pn_zigbee_remote_new ());
    TCapture   cap;
    PnMessage *msg;
    JsonObject *data;

    /* No filters set: a fresh node forwards every button press. */
    t_capture_attach (node, &cap);
    msg = press ("zigbee2mqtt/btn", "on", NULL);
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 1);
    data = pn_message_get_data (cap.last);
    CHECK (json_object_get_boolean_member (data, "success"));
    CHECK_NEAR (json_object_get_double_member (data, "value"), 1.0);
    CHECK_STR_EQ (json_object_get_string_member (data, "output"),
                  "Remote btn activated (action: on)");

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_no_action_dropped (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_remote_new ());
    TCapture cap;
    PnMessage *no_action, *no_payload;

    t_capture_attach (node, &cap);

    /* An idle / state-only publish (no `action`) is not a press. */
    no_action = press ("zigbee2mqtt/btn", NULL, NULL);
    pn_node_receive_message (node, no_action);
    CHECK_INT_EQ (cap.count, 0);

    /* A message with no payload object at all is dropped too. */
    no_payload = pn_message_new (NULL, "zigbee2mqtt/btn");
    pn_node_receive_message (node, no_payload);
    CHECK_INT_EQ (cap.count, 0);

    g_object_unref (no_action);
    g_object_unref (no_payload);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_friendly_name_filter_by_topic (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_remote_new ());
    TCapture cap;
    PnMessage *match, *other;

    g_object_set (node, "friendly-name", "remote1", NULL);
    t_capture_attach (node, &cap);

    /* Topic tail matches the configured name -> forwarded. */
    match = press ("zigbee2mqtt/remote1", "on", NULL);
    pn_node_receive_message (node, match);
    CHECK_INT_EQ (cap.count, 1);

    /* A different remote -> dropped. */
    other = press ("zigbee2mqtt/remote2", "on", NULL);
    pn_node_receive_message (node, other);
    CHECK_INT_EQ (cap.count, 1);

    g_object_unref (match);
    g_object_unref (other);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_friendly_name_via_device_block (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_remote_new ());
    TCapture cap;
    PnMessage *msg;

    /* The filter also matches the injected device.friendlyName, and the
     * output uses that human-readable name in preference to the IEEE
     * address in the topic tail. */
    g_object_set (node, "friendly-name", "Bedroom", NULL);
    t_capture_attach (node, &cap);

    msg = press ("zigbee2mqtt/0xa4c138c974b825c2", "toggle", "Bedroom");
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 1);
    CHECK_STR_EQ (json_object_get_string_member (pn_message_get_data (cap.last),
                                                 "output"),
                  "Remote Bedroom activated (action: toggle)");

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_action_filter (void)
{
    PnNode  *node = PN_NODE (pn_zigbee_remote_new ());
    TCapture cap;
    PnMessage *wrong, *right;

    g_object_set (node, "action", "toggle", NULL);
    t_capture_attach (node, &cap);

    /* A different button -> dropped. */
    wrong = press ("zigbee2mqtt/btn", "on", NULL);
    pn_node_receive_message (node, wrong);
    CHECK_INT_EQ (cap.count, 0);

    /* The configured button -> forwarded. */
    right = press ("zigbee2mqtt/btn", "toggle", NULL);
    pn_node_receive_message (node, right);
    CHECK_INT_EQ (cap.count, 1);

    g_object_unref (wrong);
    g_object_unref (right);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_metadata (void)
{
    PnNode *node = PN_NODE (pn_zigbee_remote_new ());

    CHECK (pn_node_get_has_input (node));
    CHECK (pn_node_get_has_output (node));
    CHECK_STR_EQ (pn_node_get_class_name (node), "Zigbee Remote");
    CHECK_STR_EQ (pn_node_get_category (node), "Zigbee");

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-remote");
    t_add ("forwards_press_wildcard",      test_forwards_press_wildcard);
    t_add ("no_action_dropped",            test_no_action_dropped);
    t_add ("friendly_name_filter_topic",   test_friendly_name_filter_by_topic);
    t_add ("friendly_name_via_device",     test_friendly_name_via_device_block);
    t_add ("action_filter",                test_action_filter);
    t_add ("metadata",                     test_metadata);
    return t_run ();
}
