/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * Unit tests for PnZigbeeRelayCommand: the send-only builder that turns
 * a numeric value message into a Z2M `set` command.  Pure message
 * transformation -- no I/O to stub.
 */

#include "pn-test.h"
#include "pn-zigbee-relay-command.h"

#include <json-glib/json-glib.h>

#define WARNING_ICON "\xe2\x9d\x97"   /* U+2757, the "unconfigured" glyph */

static PnNode *
make_node (const char *friendly_name)
{
    PnNode *node = PN_NODE (pn_zigbee_relay_command_new ());
    if (friendly_name != NULL)
        g_object_set (node, "friendly-name", friendly_name, NULL);
    return node;
}

/* Borrow the string `state` out of the emitted message's structured
 * data.payload object. */
static const char *
payload_state (PnMessage *msg)
{
    JsonObject *data = pn_message_get_data (msg);
    JsonObject *payload;

    if (!json_object_has_member (data, "payload"))
        return NULL;
    payload = json_object_get_object_member (data, "payload");
    if (payload == NULL || !json_object_has_member (payload, "state"))
        return NULL;
    return json_object_get_string_member (payload, "state");
}

static void
test_unconfigured_drops (void)
{
    PnNode  *node = make_node (NULL);
    TCapture cap;
    PnMessage *msg;

    t_capture_attach (node, &cap);
    msg = pn_message_new (NULL, NULL);
    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);

    /* Never command a physical device without an explicit target. */
    CHECK_INT_EQ (cap.count, 0);

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_value_high_commands_on (void)
{
    PnNode    *node = make_node ("lamp");
    TCapture   cap;
    PnMessage *msg;
    JsonObject *data;

    t_capture_attach (node, &cap);
    msg = pn_message_new (NULL, "ignored/inbound/topic");
    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 1);
    /* Topic is rewritten to the Z2M command path. */
    CHECK_STR_EQ (pn_message_get_topic (cap.last), "zigbee2mqtt/lamp/set");
    CHECK_STR_EQ (payload_state (cap.last), "ON");
    data = pn_message_get_data (cap.last);
    CHECK_NEAR (json_object_get_double_member (data, "value"), 1.0);
    CHECK (json_object_get_boolean_member (data, "success"));
    CHECK_STR_EQ (json_object_get_string_member (data, "device"), "lamp");
    CHECK_STR_EQ (json_object_get_string_member (data, "output"),
                  "lamp: command state ON");

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_value_low_commands_off (void)
{
    PnNode    *node = make_node ("lamp");
    TCapture   cap;
    PnMessage *msg;

    t_capture_attach (node, &cap);
    msg = pn_message_new (NULL, NULL);
    pn_message_set_double (msg, "value", 0.0);
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 1);
    CHECK_STR_EQ (pn_message_get_topic (cap.last), "zigbee2mqtt/lamp/set");
    CHECK_STR_EQ (payload_state (cap.last), "OFF");
    CHECK_NEAR (json_object_get_double_member (pn_message_get_data (cap.last),
                                               "value"),
                0.0);

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_threshold (void)
{
    PnNode  *node = make_node ("lamp");
    TCapture cap;
    PnMessage *m;

    t_capture_attach (node, &cap);

    /* Strictly greater than 0.5 is ON; exactly 0.5 is OFF. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", 0.6);
    pn_node_receive_message (node, m);
    CHECK_STR_EQ (payload_state (cap.last), "ON");
    g_object_unref (m);

    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", 0.5);
    pn_node_receive_message (node, m);
    CHECK_STR_EQ (payload_state (cap.last), "OFF");
    g_object_unref (m);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_integer_value_accepted (void)
{
    PnNode  *node = make_node ("lamp");
    TCapture cap;
    PnMessage *m;

    t_capture_attach (node, &cap);

    /* An int-typed value (from a counter) is accepted as well as a double. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_int (m, "value", 1);
    pn_node_receive_message (node, m);
    CHECK_INT_EQ (cap.count, 1);
    CHECK_STR_EQ (payload_state (cap.last), "ON");
    g_object_unref (m);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_no_value_dropped (void)
{
    PnNode  *node = make_node ("lamp");
    TCapture cap;
    PnMessage *m;

    t_capture_attach (node, &cap);

    /* Missing value: leave the device alone. */
    m = pn_message_new (NULL, NULL);
    pn_node_receive_message (node, m);
    CHECK_INT_EQ (cap.count, 0);
    g_object_unref (m);

    /* Non-numeric value: same. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "ON");
    pn_node_receive_message (node, m);
    CHECK_INT_EQ (cap.count, 0);
    g_object_unref (m);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_visual_state (void)
{
    PnNode *node = PN_NODE (pn_zigbee_relay_command_new ());

    CHECK_STR_EQ (pn_node_get_icon (node), WARNING_ICON);
    g_object_set (node, "friendly-name", "lamp", NULL);
    CHECK_FALSE (g_strcmp0 (pn_node_get_icon (node), WARNING_ICON) == 0);

    CHECK (pn_node_get_has_input (node));
    CHECK (pn_node_get_has_output (node));
    CHECK_STR_EQ (pn_node_get_class_name (node), "Zigbee Relay Command");
    CHECK_STR_EQ (pn_node_get_category (node), "Zigbee");

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-relay-command");
    t_add ("unconfigured_drops",       test_unconfigured_drops);
    t_add ("value_high_commands_on",   test_value_high_commands_on);
    t_add ("value_low_commands_off",   test_value_low_commands_off);
    t_add ("threshold",                test_threshold);
    t_add ("integer_value_accepted",   test_integer_value_accepted);
    t_add ("no_value_dropped",         test_no_value_dropped);
    t_add ("visual_state",             test_visual_state);
    return t_run ();
}
