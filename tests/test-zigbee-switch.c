/*
 * Copyright (C) 2026 Laszlo Pere
 *
 * This file is part of pipnode-zigbee-plugin, a plugin for Pipnode, and
 * is free software under the GNU General Public License version 3 or (at
 * your option) any later version.  See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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

    if (state != NULL)
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
test_inbound_non_binary_state_logs (void)
{
    PnNode   *node = PN_NODE (pn_zigbee_switch_new ());
    PnSwitch *sw   = PN_SWITCH (node);
    TCapture  cap;
    PnMessage *toggle, *empty;

    g_object_set (node, "friendly-name", "lamp", NULL);
    t_capture_attach (node, &cap);

    /* A present-but-non-binary inbound state (e.g. "TOGGLE") leaves the
     * latch alone and emits nothing, but is bounded and diagnostic, so
     * it is logged at INFO naming the value. */
    toggle = state_publish ("zigbee2mqtt/lamp", "TOGGLE");
    pn_node_receive_message (node, toggle);
    CHECK_FALSE (pn_switch_get_on (sw));
    CHECK_INT_EQ (cap.count, 0);
    CHECK_INT_EQ (t_log_count (node, PN_LOG_LEVEL_INFO), 1);
    CHECK (t_log_contains (node, PN_LOG_LEVEL_INFO, "TOGGLE"));

    /* A payload with no `state` member is the routine attribute-update
     * case (Z2M publishes brightness/linkquality on this same topic),
     * so it is dropped WITHOUT logging -- nothing new is recorded. */
    empty = state_publish ("zigbee2mqtt/lamp", NULL);
    pn_node_receive_message (node, empty);
    CHECK_INT_EQ (cap.count, 0);
    CHECK_INT_EQ (t_log_total (node), 1);

    g_object_unref (toggle);
    g_object_unref (empty);
    t_capture_clear (&cap);
    g_object_unref (node);
}

static void
test_unconfigured_outbound_logs (void)
{
    PnNode   *node = PN_NODE (pn_zigbee_switch_new ());
    PnSwitch *sw   = PN_SWITCH (node);
    TCapture  cap;

    /* No friendly-name set: a toggle still emits (the base switch
     * shape, via the dispatcher) but cannot build a real
     * zigbee2mqtt/<dev>/set command.  Unlike the inbound read path,
     * this fires only on a user toggle, so it is surfaced as a WARNING. */
    t_capture_attach (node, &cap);
    pn_switch_toggle (sw);

    CHECK_INT_EQ (cap.count, 1);
    /* The fallback's whole purpose is to avoid publishing a command to
     * the empty "zigbee2mqtt//set" topic; whatever base topic it emits,
     * it must not be that. */
    CHECK (g_strcmp0 (pn_message_get_topic (cap.last),
                      "zigbee2mqtt//set") != 0);
    CHECK_INT_EQ (t_log_count (node, PN_LOG_LEVEL_WARNING), 1);
    CHECK (t_log_contains (node, PN_LOG_LEVEL_WARNING,
                           "no friendly_name configured"));

    t_capture_clear (&cap);
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
    CHECK_STR_EQ (pn_node_get_class_name (node), "Zigbee Switch");

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-switch");
    t_add ("inbound_syncs_latch_silently", test_inbound_syncs_latch_silently);
    t_add ("inbound_wrong_topic_ignored",  test_inbound_wrong_topic_ignored);
    t_add ("inbound_unconfigured_noop",     test_inbound_unconfigured_noop);
    t_add ("inbound_non_binary_state_logs", test_inbound_non_binary_state_logs);
    t_add ("unconfigured_outbound_logs",    test_unconfigured_outbound_logs);
    t_add ("outbound_toggle_builds_cmd",    test_outbound_toggle_builds_command);
    t_add ("visual_state",                  test_visual_state);
    return t_run ();
}
