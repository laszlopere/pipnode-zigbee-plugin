/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * Unit tests for PnZigbeeRelayStatus: the receive-only decoder that
 * turns a Z2M state publish into the canonical value/success/output
 * message shape.  Pure message transformation -- no I/O to stub.
 */

#include "pn-test.h"
#include "pn-zigbee-relay-status.h"

#include <json-glib/json-glib.h>

#define WARNING_ICON "\xe2\x9d\x97"   /* U+2757, the "unconfigured" glyph */

/* Build a Z2M-style state publish: topic + structured data.payload
 * carrying a string `state` member, the shape the node decodes. */
static PnMessage *
state_publish (const char *topic, const char *state)
{
    PnMessage  *msg = pn_message_new (NULL, topic);
    JsonObject *payload = json_object_new ();
    JsonNode   *node    = json_node_new (JSON_NODE_OBJECT);

    if (state != NULL)
        json_object_set_string_member (payload, "state", state);
    json_node_take_object (node, payload);
    pn_message_set_member (msg, "payload", node);
    return msg;
}

static PnNode *
make_node (const char *friendly_name)
{
    PnNode *node = PN_NODE (pn_zigbee_relay_status_new ());
    if (friendly_name != NULL)
        g_object_set (node, "friendly-name", friendly_name, NULL);
    return node;
}

static void
test_unconfigured_drops (void)
{
    PnNode  *node = make_node (NULL);
    TCapture cap;
    PnMessage *msg;

    t_capture_attach (node, &cap);
    msg = state_publish ("zigbee2mqtt/lamp", "ON");
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 0);

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_forwards_on (void)
{
    PnNode    *node = make_node ("lamp");
    TCapture   cap;
    PnMessage *msg;
    JsonObject *data;

    t_capture_attach (node, &cap);
    msg = state_publish ("zigbee2mqtt/lamp", "ON");
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 1);
    CHECK_NOT_NULL (cap.last);
    data = pn_message_get_data (cap.last);
    CHECK_NEAR (json_object_get_double_member (data, "value"), 1.0);
    CHECK (json_object_get_boolean_member (data, "success"));
    CHECK_STR_EQ (json_object_get_string_member (data, "device"), "lamp");
    CHECK_STR_EQ (json_object_get_string_member (data, "output"),
                  "lamp: state ON");
    /* Topic is left intact -- a status decoder never rewrites it. */
    CHECK_STR_EQ (pn_message_get_topic (cap.last), "zigbee2mqtt/lamp");

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_forwards_off (void)
{
    PnNode    *node = make_node ("lamp");
    TCapture   cap;
    PnMessage *msg;
    JsonObject *data;

    t_capture_attach (node, &cap);
    msg = state_publish ("zigbee2mqtt/lamp", "OFF");
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 1);
    data = pn_message_get_data (cap.last);
    CHECK_NEAR (json_object_get_double_member (data, "value"), 0.0);
    CHECK_STR_EQ (json_object_get_string_member (data, "output"),
                  "lamp: state OFF");

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_wrong_topic_dropped (void)
{
    PnNode  *node = make_node ("lamp");
    TCapture cap;
    PnMessage *msg;

    t_capture_attach (node, &cap);
    /* A sibling endpoint's publish must not be stolen. */
    msg = state_publish ("zigbee2mqtt/lamp_bedroom", "ON");
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 0);

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_non_binary_state_dropped (void)
{
    PnNode  *node = make_node ("lamp");
    TCapture cap;
    PnMessage *toggle, *empty;

    t_capture_attach (node, &cap);

    /* "TOGGLE" is neither ON nor OFF: silently dropped, no synthesised 0. */
    toggle = state_publish ("zigbee2mqtt/lamp", "TOGGLE");
    pn_node_receive_message (node, toggle);
    CHECK_INT_EQ (cap.count, 0);

    /* A payload object with no `state` member is dropped too. */
    empty = state_publish ("zigbee2mqtt/lamp", NULL);
    pn_node_receive_message (node, empty);
    CHECK_INT_EQ (cap.count, 0);

    g_object_unref (toggle);
    g_object_unref (empty);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_state_match_is_case_insensitive (void)
{
    PnNode  *node = make_node ("lamp");
    TCapture cap;
    PnMessage *msg;

    t_capture_attach (node, &cap);
    msg = state_publish ("zigbee2mqtt/lamp", "on");
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 1);
    CHECK_NEAR (json_object_get_double_member (pn_message_get_data (cap.last),
                                               "value"),
                1.0);

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_visual_state (void)
{
    PnNode *node = PN_NODE (pn_zigbee_relay_status_new ());

    /* Fresh, unconfigured node nags with the warning glyph. */
    CHECK_STR_EQ (pn_node_get_icon (node), WARNING_ICON);

    /* Once a target is set it drops the warning. */
    g_object_set (node, "friendly-name", "lamp", NULL);
    CHECK_FALSE (g_strcmp0 (pn_node_get_icon (node), WARNING_ICON) == 0);

    /* Metadata: a read+forward node has both ports. */
    CHECK (pn_node_get_has_input (node));
    CHECK (pn_node_get_has_output (node));
    CHECK_STR_EQ (pn_node_get_class_name (node), "Zigbee Relay Status");
    CHECK_STR_EQ (pn_node_get_category (node), "Zigbee");

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-relay-status");
    t_add ("unconfigured_drops",            test_unconfigured_drops);
    t_add ("forwards_on",                   test_forwards_on);
    t_add ("forwards_off",                  test_forwards_off);
    t_add ("wrong_topic_dropped",           test_wrong_topic_dropped);
    t_add ("non_binary_state_dropped",      test_non_binary_state_dropped);
    t_add ("state_match_case_insensitive",  test_state_match_is_case_insensitive);
    t_add ("visual_state",                  test_visual_state);
    return t_run ();
}
