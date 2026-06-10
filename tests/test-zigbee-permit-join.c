/*
 * Copyright (C) 2026 Laszlo Pere
 *
 * This file is part of pipnode-zigbee-plugin, a plugin for Pipnode, and
 * is free software under the GNU General Public License version 3 or (at
 * your option) any later version.  See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit tests for PnZigbeePermitJoin: turns any inbound message (a pure
 * trigger) into a Z2M `bridge/request/permit_join` command that opens the
 * pairing window for the configured number of minutes.  Pure message
 * transformation -- no I/O to stub.
 */

#include "pn-test.h"
#include "pn-zigbee-permit-join.h"

#include <json-glib/json-glib.h>

#define PLUS_CIRCLE_ICON "\xef\x81\x95"   /* U+F055, the node's glyph */

/* A bare trigger message: the node ignores its contents entirely. */
static PnMessage *
trigger (void)
{
    return pn_message_new (NULL, "ignored/inbound/topic");
}

/* Borrow the emitted message's structured `data.payload` object (the Z2M
 * request body the node builds). */
static JsonObject *
payload_object (PnMessage *msg)
{
    JsonNode *node = pn_message_get_member (msg, "payload");

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
        return NULL;
    return json_node_get_object (node);
}

static void
test_default_window_emitted (void)
{
    PnNode    *node = PN_NODE (pn_zigbee_permit_join_new ());
    TCapture   cap;
    PnMessage *msg;
    JsonObject *payload;
    JsonObject *data;

    t_capture_attach (node, &cap);
    msg = trigger ();
    pn_node_receive_message (node, msg);

    /* Every message opens the window -- one request out. */
    CHECK_INT_EQ (cap.count, 1);

    /* Topic is rewritten to the Z2M bridge permit_join request path. */
    CHECK_STR_EQ (pn_message_get_topic (cap.last),
                  "zigbee2mqtt/bridge/request/permit_join");

    /* Payload is the structured { "value": true, "time": <seconds> }
     * form; default 4 minutes -> 240 seconds. */
    payload = payload_object (cap.last);
    CHECK_NOT_NULL (payload);
    CHECK (json_object_get_boolean_member (payload, "value"));
    CHECK_INT_EQ (json_object_get_int_member (payload, "time"), 240);

    /* Canonical value mirror rides along on data for parallel observers. */
    data = pn_message_get_data (cap.last);
    CHECK_NEAR (json_object_get_double_member (data, "value"), 240.0);
    CHECK (json_object_get_boolean_member (data, "success"));

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_minutes_converted_to_seconds (void)
{
    PnNode    *node = PN_NODE (pn_zigbee_permit_join_new ());
    TCapture   cap;
    PnMessage *msg;
    JsonObject *payload;

    g_object_set (node, "minutes", 10u, NULL);

    t_capture_attach (node, &cap);
    msg = trigger ();
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 1);
    payload = payload_object (cap.last);
    /* 10 minutes -> 600 seconds. */
    CHECK_INT_EQ (json_object_get_int_member (payload, "time"), 600);

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_any_message_triggers (void)
{
    PnNode    *node = PN_NODE (pn_zigbee_permit_join_new ());
    TCapture   cap;
    PnMessage *m;

    t_capture_attach (node, &cap);

    /* A message with no value member at all is still a valid trigger:
     * this is a fire-on-any-input node, not a value filter. */
    m = pn_message_new (NULL, NULL);
    pn_node_receive_message (node, m);
    CHECK_INT_EQ (cap.count, 1);
    g_object_unref (m);

    /* A second trigger opens the window again. */
    m = trigger ();
    pn_node_receive_message (node, m);
    CHECK_INT_EQ (cap.count, 2);
    g_object_unref (m);

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_zero_minutes_falls_back_to_default (void)
{
    PnNode    *node = PN_NODE (pn_zigbee_permit_join_new ());
    TCapture   cap;
    PnMessage *msg;
    JsonObject *payload;

    /* 0 minutes is non-positive; the node clamps it to the 4 min default
     * rather than emitting a time:0 (which would close the window). */
    g_object_set (node, "minutes", 0u, NULL);

    t_capture_attach (node, &cap);
    msg = trigger ();
    pn_node_receive_message (node, msg);

    CHECK_INT_EQ (cap.count, 1);
    payload = payload_object (cap.last);
    CHECK_INT_EQ (json_object_get_int_member (payload, "time"), 240);

    g_object_unref (msg);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_metadata (void)
{
    PnNode *node = PN_NODE (pn_zigbee_permit_join_new ());
    guint   minutes;

    /* No "needs configuration" state: a freshly-dropped node has a usable
     * default window, so it paints its own glyph and raises no error. */
    CHECK_FALSE (pn_node_get_has_error (node));
    CHECK_STR_EQ (pn_node_get_icon (node), PLUS_CIRCLE_ICON);

    CHECK (pn_node_get_has_input (node));
    CHECK (pn_node_get_has_output (node));
    CHECK_STR_EQ (pn_node_get_class_name (node), "Zigbee Permit Join");
    CHECK_STR_EQ (pn_node_get_category (node), "Zigbee");

    /* Default window is 4 minutes. */
    g_object_get (node, "minutes", &minutes, NULL);
    CHECK_INT_EQ (minutes, 4);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-permit-join");
    t_add ("default_window_emitted",        test_default_window_emitted);
    t_add ("minutes_converted_to_seconds",  test_minutes_converted_to_seconds);
    t_add ("any_message_triggers",          test_any_message_triggers);
    t_add ("zero_minutes_falls_back",       test_zero_minutes_falls_back_to_default);
    t_add ("metadata",                      test_metadata);
    return t_run ();
}
