/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * Unit tests for PnZnpPing.  This node talks ZNP/UNPI over a serial
 * port; all of that I/O (open/flock/ioctl/termios/read/write, the
 * GIOChannel watch and the poll timer) is deferred to a g_idle handler
 * the node schedules at construction and is driven entirely off the
 * GLib main loop.  These tests never pump a main loop, so that handler
 * never fires and the suite opens no device and generates no traffic.
 *
 * What we verify headlessly is the configuration surface, node
 * metadata, and the open-failure diagnostic path.  Most cases read the
 * default "device" rather than set it, since setting it triggers an
 * immediate (synchronous) reopen; the open-failure case uses exactly
 * that, pointing the port at a path that cannot exist so open() fails
 * at once without arming any watch or timer.  The frame codec (FCS,
 * parsing) is file-static in pn-znp-ping.c and not exercised here.
 */

#include "pn-test.h"
#include "pn-znp-ping.h"

#include <json-glib/json-glib.h>

#define NORMAL_ICON "\xef\x82\x9e"  /* U+F09E, the healthy radio glyph */

static void
test_metadata (void)
{
    PnNode *node = PN_NODE (pn_znp_ping_new ());

    /* A liveness probe is a pure source: output only, no input. */
    CHECK_FALSE (pn_node_get_has_input (node));
    CHECK (pn_node_get_has_output (node));
    CHECK_STR_EQ (pn_node_get_class_name (node), "ZNP Ping");
    CHECK_STR_EQ (pn_node_get_category (node), "Zigbee");

    g_object_unref (node);
}

static void
test_defaults (void)
{
    PnNode *node = PN_NODE (pn_znp_ping_new ());
    gchar  *device = NULL;
    guint   interval = 0;

    g_object_get (node,
                  "device",   &device,
                  "interval", &interval,
                  NULL);

    CHECK_STR_EQ (device, "/dev/ttyUSB0");
    CHECK_INT_EQ (interval, 5);
    /* A fresh probe has not yet confirmed the dongle is alive, so it
     * raises the host error overlay (red ❗); the node keeps its healthy
     * radio glyph, and a successful PING later clears the error. */
    CHECK (pn_node_get_has_error (node));
    CHECK_STR_EQ (pn_node_get_icon (node), NORMAL_ICON);

    g_free (device);
    g_object_unref (node);
}

static void
test_interval_round_trips (void)
{
    PnNode *node = PN_NODE (pn_znp_ping_new ());
    guint   interval = 0;

    /* Setting the interval reschedules a timer but does not open the
     * port, so it stays I/O-free; the value must round-trip. */
    g_object_set (node, "interval", 30u, NULL);
    g_object_get (node, "interval", &interval, NULL);
    CHECK_INT_EQ (interval, 30);

    g_object_unref (node);
}

static void
test_open_failure_logs_error (void)
{
    PnNode  *node = PN_NODE (pn_znp_ping_new ());
    TCapture cap;

    t_capture_attach (node, &cap);

    /* Setting "device" reopens the port synchronously (the one piece of
     * I/O the node does without a main loop).  Pointing it at a path
     * that cannot exist makes open() fail immediately; that failure arms
     * no watch and no timer, so the test stays headless.  Every failure
     * path funnels its reason through emit_failure, which now mirrors it
     * to the node's diagnostic log at ERROR as well as emitting it
     * downstream. */
    g_object_set (node, "device", "/nonexistent-pn-zigbee-test/ttyZZ", NULL);

    /* Downstream failure signal still goes out (success = FALSE) ... */
    CHECK_INT_EQ (cap.count, 1);
    CHECK_FALSE (json_object_get_boolean_member (pn_message_get_data (cap.last),
                                                 "success"));
    /* ... and the same reason is now visible in the node's log. */
    CHECK_INT_EQ (t_log_count (node, PN_LOG_LEVEL_ERROR), 1);
    CHECK (t_log_contains (node, PN_LOG_LEVEL_ERROR, "open("));

    t_capture_clear (&cap);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-znp-ping");
    t_add ("metadata",            test_metadata);
    t_add ("defaults",            test_defaults);
    t_add ("interval_round_trips", test_interval_round_trips);
    t_add ("open_failure_logs_error", test_open_failure_logs_error);
    return t_run ();
}
