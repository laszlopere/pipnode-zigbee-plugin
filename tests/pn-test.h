/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * Tiny self-contained unit-test harness for the Zigbee plugin.  Written
 * from scratch for this (proprietary) plugin -- it shares nothing with
 * the GPL pipnode tree beyond the public node / message API it drives.
 *
 * A test binary registers named cases with t_add() and calls t_run()
 * from main(); each case fires CHECK_* assertions.  Output is a brief
 * per-case tally by default and a per-check PASS/FAIL trace with -v:
 *
 *     pn-zigbee-relay-status
 *       unconfigured_drops            1 checks, 0 failed
 *       forwards_on                   6 checks, 0 failed
 *       total: 7 checks, 0 failed
 *
 * Tests are headless: a node is built with its pn_*_new() constructor,
 * fed PnMessage objects via pn_node_receive_message(), and its emitted
 * messages are captured off the "message" signal.  No main loop is ever
 * pumped, so deferred/timer-driven I/O (serial open, MQTT connect) never
 * runs and the suite touches neither the network nor the filesystem.
 */

#ifndef PN_ZIGBEE_TEST_H
#define PN_ZIGBEE_TEST_H

#include <glib.h>
#include <math.h>

#include <pn-node.h>
#include <pn-message.h>

G_BEGIN_DECLS

typedef void (*TCaseFunc) (void);

/* Harness lifecycle. */
void t_init   (int *argc, char ***argv, const char *suite);
void t_add    (const char *name, TCaseFunc func);
int  t_run    (void);   /* exit code: 0 all-pass, 1 if any check failed */

/* One assertion result; macros below feed it.  Not called directly. */
void t_report (gboolean ok, const char *expr, const char *file, int line);

#define CHECK(expr) \
    t_report ((expr) ? TRUE : FALSE, #expr, __FILE__, __LINE__)
#define CHECK_FALSE(expr) \
    t_report ((expr) ? FALSE : TRUE, "!(" #expr ")", __FILE__, __LINE__)
#define CHECK_INT_EQ(a, b) \
    t_report ((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_STR_EQ(a, b) \
    t_report (g_strcmp0 ((a), (b)) == 0, #a " == " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b) \
    t_report (fabs ((double) (a) - (double) (b)) < 1e-9, \
              #a " ~= " #b, __FILE__, __LINE__)
#define CHECK_NULL(p) \
    t_report ((p) == NULL, #p " == NULL", __FILE__, __LINE__)
#define CHECK_NOT_NULL(p) \
    t_report ((p) != NULL, #p " != NULL", __FILE__, __LINE__)

/* ------------------------------------------------------------------ */
/*  Emitted-message capture                                            */
/*                                                                     */
/*  Attach to a node to count and keep its most recently emitted       */
/*  message; the harness owns the kept ref and drops it on the next    */
/*  emit or on t_capture_clear().                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    guint      count;   /* number of "message" emissions seen */
    PnMessage *last;    /* most recent emission (owned), or NULL */
} TCapture;

void t_capture_attach (PnNode *node, TCapture *cap);
void t_capture_clear  (TCapture *cap);

G_END_DECLS

#endif /* PN_ZIGBEE_TEST_H */
