/*
 * Copyright (C) 2026 Laszlo Pere
 *
 * This file is part of pipnode-zigbee-plugin, a plugin for Pipnode, and
 * is free software under the GNU General Public License version 3 or (at
 * your option) any later version.  See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implementation of the tiny unit-test harness declared in pn-test.h.
 * Original to this plugin; see the header for the contract and output
 * format.
 */

#include "pn-test.h"

#define T_NAME_WIDTH 28
#define T_MAX_CASES  128

typedef struct {
    const char *name;
    TCaseFunc   func;
} TCase;

static const char *t_suite;
static gboolean    t_verbose;

static TCase t_cases[T_MAX_CASES];
static guint t_n_cases;

/* Counters for the case currently running, plus suite-wide totals. */
static guint t_cur_checks;
static guint t_cur_failed;
static guint t_total_checks;
static guint t_total_failed;

void
t_init (int *argc, char ***argv, const char *suite)
{
    t_suite = suite;

    /* Strip a -v / --verbose flag if present; ignore everything else so
     * the runner can pass through arguments without upsetting us. */
    if (argc != NULL && argv != NULL)
    {
        for (int i = 1; i < *argc; i++)
        {
            const char *a = (*argv)[i];
            if (g_strcmp0 (a, "-v") == 0 || g_strcmp0 (a, "--verbose") == 0)
                t_verbose = TRUE;
        }
    }
}

void
t_add (const char *name, TCaseFunc func)
{
    g_assert (t_n_cases < T_MAX_CASES);
    t_cases[t_n_cases].name = name;
    t_cases[t_n_cases].func = func;
    t_n_cases++;
}

void
t_report (gboolean ok, const char *expr, const char *file, int line)
{
    t_cur_checks++;
    t_total_checks++;

    if (!ok)
    {
        t_cur_failed++;
        t_total_failed++;
        if (t_verbose)
            g_print ("      FAIL  %s  (%s:%d)\n", expr, file, line);
    }
    else if (t_verbose)
    {
        g_print ("      PASS  %s\n", expr);
    }
}

int
t_run (void)
{
    g_print ("%s\n", t_suite);

    for (guint i = 0; i < t_n_cases; i++)
    {
        t_cur_checks = 0;
        t_cur_failed = 0;

        if (t_verbose)
            g_print ("  %s\n", t_cases[i].name);

        t_cases[i].func ();

        g_print ("  %-*s  %u checks, %u failed\n",
                 T_NAME_WIDTH, t_cases[i].name,
                 t_cur_checks, t_cur_failed);
    }

    g_print ("  total: %u checks, %u failed\n",
             t_total_checks, t_total_failed);

    return t_total_failed > 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Emitted-message capture                                            */
/* ------------------------------------------------------------------ */

static void
t_on_message (PnNode *node, PnMessage *message, gpointer user_data)
{
    TCapture *cap = user_data;

    (void) node;

    cap->count++;
    if (cap->last != NULL)
        g_object_unref (cap->last);
    cap->last = message != NULL ? g_object_ref (message) : NULL;
}

void
t_capture_attach (PnNode *node, TCapture *cap)
{
    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (t_on_message), cap);
}

void
t_capture_clear (TCapture *cap)
{
    if (cap->last != NULL)
    {
        g_object_unref (cap->last);
        cap->last = NULL;
    }
    cap->count = 0;
}

/* ------------------------------------------------------------------ */
/*  Diagnostic-log inspection                                          */
/* ------------------------------------------------------------------ */

guint
t_log_total (PnNode *node)
{
    GPtrArray *log = pn_node_get_log (node);
    return log != NULL ? log->len : 0;
}

guint
t_log_count (PnNode *node, PnLogLevel level)
{
    GPtrArray *log = pn_node_get_log (node);
    guint      n   = 0;

    if (log == NULL)
        return 0;
    for (guint i = 0; i < log->len; i++)
        if (pn_log_entry_get_level (log->pdata[i]) == level)
            n++;
    return n;
}

gboolean
t_log_contains (PnNode *node, PnLogLevel level, const char *substr)
{
    GPtrArray *log = pn_node_get_log (node);

    if (log == NULL)
        return FALSE;
    for (guint i = 0; i < log->len; i++)
    {
        PnLogEntry *e = log->pdata[i];
        if (pn_log_entry_get_level (e) == level &&
            g_strstr_len (pn_log_entry_get_message (e), -1, substr) != NULL)
            return TRUE;
    }
    return FALSE;
}
