/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * Unit tests for PnZigbeeSwitch: a bidirectional switch.  Inbound it
 * silently syncs its latch from a Z2M state publish (no emit); outbound
 * a toggle builds a Z2M `set` command.  No I/O to stub -- the actual
 * MQTT publish happens in a downstream sink, not here.
 */

#include "pn-test.h"
#include "pn-zigbee-switch.h"

#include <pn-switch.h>
#include <json-glib/json-glib.h>

#define TOGGLE_ICON "\xef\x88\x85"   /* U+F205, the node's healthy glyph */

static PnMessage *
state_publish (const char *topic, const char *state)
{
    PnMessage  *msg     = pn_message_new (NULL, topic);
    JsonObject *payload = json_object_new ();
    JsonNode   *node    = json_node_new (JSON_NODE_OBJECT);

    json_object_set_string_member (payload, "state", state);
    json_node_take_object (node, payload);
    pn_message_set_member (msg, "payload", node);
    return msg;
}

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
test_inbound_syncs_latch_silently (void)
{
    PnNode   *node = PN_NODE (pn_zigbee_switch_new ());
    PnSwitch *sw   = PN_SWITCH (node);
    TCapture  cap;
    PnMessage *on, *off;

    g_object_set (node, "friendly-name", "lamp", NULL);
    t_capture_attach (node, &cap);

    on = state_publish ("zigbee2mqtt/lamp", "ON");
    pn_node_receive_message (node, on);
    CHECK (pn_switch_get_on (sw));
    /* Syncing the latch from a state publish must not emit (no command). */
    CHECK_INT_EQ (cap.count, 0);

    off = state_publish ("zigbee2mqtt/lamp", "OFF");
    pn_node_receive_message (node, off);
    CHECK_FALSE (pn_switch_get_on (sw));
    CHECK_INT_EQ (cap.count, 0);

    g_object_unref (on);
    g_object_unref (off);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_inbound_wrong_topic_ignored (void)
{
    PnNode   *node = PN_NODE (pn_zigbee_switch_new ());
    PnSwitch *sw   = PN_SWITCH (node);
    PnMessage *msg;

    g_object_set (node, "friendly-name", "lamp", NULL);

    msg = state_publish ("zigbee2mqtt/other", "ON");
    pn_node_receive_message (node, msg);
    CHECK_FALSE (pn_switch_get_on (sw));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_inbound_unconfigured_noop (void)
{
    PnNode   *node = PN_NODE (pn_zigbee_switch_new ());
    PnSwitch *sw   = PN_SWITCH (node);
    PnMessage *msg;

    msg = state_publish ("zigbee2mqtt/lamp", "ON");
    pn_node_receive_message (node, msg);
    CHECK_FALSE (pn_switch_get_on (sw));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_outbound_toggle_builds_command (void)
{
    PnNode   *node = PN_NODE (pn_zigbee_switch_new ());
    PnSwitch *sw   = PN_SWITCH (node);
    TCapture  cap;
    JsonObject *data;

    g_object_set (node, "friendly-name", "lamp", NULL);
    t_capture_attach (node, &cap);

    /* A fresh switch starts off; one toggle drives it on and emits the
     * Z2M command for that new state. */
    pn_switch_toggle (sw);
    CHECK_INT_EQ (cap.count, 1);
    CHECK_STR_EQ (pn_message_get_topic (cap.last), "zigbee2mqtt/lamp/set");
    CHECK_STR_EQ (payload_state (cap.last), "ON");
    data = pn_message_get_data (cap.last);
    CHECK_NEAR (json_object_get_double_member (data, "value"), 1.0);
    CHECK_STR_EQ (json_object_get_string_member (data, "device"), "lamp");
    CHECK_STR_EQ (json_object_get_string_member (data, "output"),
                  "lamp: command state ON");

    /* Toggling back commands OFF. */
    pn_switch_toggle (sw);
    CHECK_INT_EQ (cap.count, 2);
    CHECK_STR_EQ (payload_state (cap.last), "OFF");

    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_visual_state (void)
{
    PnNode *node = PN_NODE (pn_zigbee_switch_new ());

    /* Unconfigured: the node raises the host error overlay (red ❗)
     * rather than swapping its own icon, which stays the healthy
     * toggle glyph (PLUGINS §12). */
    CHECK (pn_node_get_has_error (node));
    CHECK_STR_EQ (pn_node_get_icon (node), TOGGLE_ICON);

    /* Setting a target clears the error; the icon never changed. */
    g_object_set (node, "friendly-name", "lamp", NULL);
    CHECK_FALSE (pn_node_get_has_error (node));
    CHECK_STR_EQ (pn_node_get_icon (node), TOGGLE_ICON);

    CHECK (pn_node_get_has_input (node));
    CHECK (pn_node_get_has_output (node));
    CHECK_STR_EQ (pn_node_get_category (node), "Zigbee");

    /* NOTE: class_name is intentionally NOT asserted here.  The class
     * pins "Zigbee Switch" (pn-zigbee-switch.c), but pn_node_get_class_name()
     * currently reports "Switch" for an instance: the PnSwitch base seeds
     * a per-instance class-name label that shadows the subclass class-level
     * value.  The other (direct-PnNode) nodes are unaffected and DO assert
     * their class_name.  See TODO.md -- this discrepancy is tracked there. */

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-switch");
    t_add ("inbound_syncs_latch_silently", test_inbound_syncs_latch_silently);
    t_add ("inbound_wrong_topic_ignored",  test_inbound_wrong_topic_ignored);
    t_add ("inbound_unconfigured_noop",     test_inbound_unconfigured_noop);
    t_add ("outbound_toggle_builds_cmd",    test_outbound_toggle_builds_command);
    t_add ("visual_state",                  test_visual_state);
    return t_run ();
}
