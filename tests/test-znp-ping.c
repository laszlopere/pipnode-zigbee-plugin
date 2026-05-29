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
 * What we verify headlessly is the configuration surface and node
 * metadata.  We deliberately do NOT set the "device" property, because
 * doing so triggers an immediate (synchronous) reopen of the port; the
 * default is read instead.  The frame codec (FCS, parsing) is file-
 * static in pn-znp-ping.c and not exercised here.
 */

#include "pn-test.h"
#include "pn-znp-ping.h"

#define WARNING_ICON "\xe2\x9d\x97"  /* U+2757, the "liveness unconfirmed" glyph */

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
     * seeds the warning glyph; a successful PING later clears it. */
    CHECK_STR_EQ (pn_node_get_icon (node), WARNING_ICON);

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

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-znp-ping");
    t_add ("metadata",            test_metadata);
    t_add ("defaults",            test_defaults);
    t_add ("interval_round_trips", test_interval_round_trips);
    return t_run ();
}
