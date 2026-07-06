/*
 * Copyright (C) 2026 Laszlo Pere
 *
 * This file is part of pipnode-zigbee-plugin, a plugin for Pipnode, and
 * is free software under the GNU General Public License version 3 or (at
 * your option) any later version.  See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit tests for the background friendly-name registry.  The registry
 * silently gathers Zigbee2MQTT device names from every broker so a node's
 * friendly-name field can offer them as a dropdown.  The ingest half is
 * GTK-free and drives off a plain bridge/devices JSON payload, so this
 * suite exercises it directly with zb_name_registry_ingest_devices() and
 * inspects the result via zb_name_registry_get_names() -- no broker, no
 * network, no GLib main loop, no GTK.  zb_name_registry_start() (which
 * would construct the hidden PnMqtt sources) is intentionally never
 * called; only the parse/merge/dedup/sort logic is under test.
 *
 * The registry is a process-global singleton whose set only ever grows, so
 * cases use distinctive names and assert membership / per-name properties
 * rather than exact whole-list equality, staying robust to accumulation
 * across cases.
 */

#include "pn-test.h"
#include "pn-zigbee-name-registry.h"

#include <json-glib/json-glib.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Parse @json into an owned JsonNode (or NULL on bad JSON). */
static JsonNode *
parse (const char *json)
{
    return json_from_string (json, NULL);
}

/* Ingest a bridge/devices payload given as a JSON string, freeing it. */
static void
ingest (const char *json)
{
    JsonNode *node = parse (json);
    zb_name_registry_ingest_devices (node);
    if (node != NULL)
        json_node_unref (node);
}

/* Is @name currently in the registry snapshot? */
static gboolean
names_contains (const char *name)
{
    GPtrArray *names = zb_name_registry_get_names ();
    gboolean   found = FALSE;
    guint      i;

    for (i = 0; i < names->len; i++)
        if (g_strcmp0 (g_ptr_array_index (names, i), name) == 0)
        {
            found = TRUE;
            break;
        }
    g_ptr_array_unref (names);
    return found;
}

/* How many times @name appears in the snapshot (0 or, if dedup fails, >1). */
static guint
names_count_of (const char *name)
{
    GPtrArray *names = zb_name_registry_get_names ();
    guint      hits = 0, i;

    for (i = 0; i < names->len; i++)
        if (g_strcmp0 (g_ptr_array_index (names, i), name) == 0)
            hits++;
    g_ptr_array_unref (names);
    return hits;
}

/* ------------------------------------------------------------------ */
/*  Cases                                                              */
/* ------------------------------------------------------------------ */

/* A well-formed inventory contributes every device's friendly_name. */
static void
case_adds_names (void)
{
    ingest ("[{\"friendly_name\":\"reg_lamp\"},"
            " {\"friendly_name\":\"reg_plug\"}]");

    CHECK (names_contains ("reg_lamp"));
    CHECK (names_contains ("reg_plug"));
}

/* Ingesting the same name twice keeps a single entry (set semantics). */
static void
case_dedups (void)
{
    ingest ("[{\"friendly_name\":\"reg_dup\"}]");
    ingest ("[{\"friendly_name\":\"reg_dup\"}]");

    CHECK_INT_EQ (names_count_of ("reg_dup"), 1);
}

/* Names from separate publishes (as if from different brokers) merge into
 * one set rather than replacing each other. */
static void
case_merges_across_publishes (void)
{
    ingest ("[{\"friendly_name\":\"reg_broker_a\"}]");
    ingest ("[{\"friendly_name\":\"reg_broker_b\"}]");

    CHECK (names_contains ("reg_broker_a"));
    CHECK (names_contains ("reg_broker_b"));
}

/* Empty, missing, and non-string friendly_name entries are skipped; a
 * usable sibling in the same array still lands. */
static void
case_skips_unusable (void)
{
    ingest ("[{\"friendly_name\":\"\"},"
            " {\"ieee_address\":\"0xabc\"},"
            " {\"friendly_name\":123},"
            " {\"friendly_name\":\"reg_usable\"}]");

    CHECK (names_contains ("reg_usable"));
    CHECK_FALSE (names_contains (""));      /* empty name never offered */
}

/* Malformed payloads (NULL, non-array, array of scalars) are ignored
 * without crashing and never inject a spurious entry; a good publish still
 * works afterward. */
static void
case_ignores_malformed (void)
{
    zb_name_registry_ingest_devices (NULL);
    ingest ("{\"friendly_name\":\"not_an_array\"}");   /* object, not array */
    ingest ("[\"reg_scalar\", 7, null]");              /* array of scalars  */

    CHECK_FALSE (names_contains ("not_an_array"));
    CHECK_FALSE (names_contains ("reg_scalar"));

    /* The registry is still healthy after the bad input. */
    ingest ("[{\"friendly_name\":\"reg_after_bad\"}]");
    CHECK (names_contains ("reg_after_bad"));
}

/* The snapshot is returned in ascending name order regardless of the order
 * names were ingested in. */
static void
case_sorted (void)
{
    GPtrArray *names;
    guint      i;

    ingest ("[{\"friendly_name\":\"reg_zzz\"},"
            " {\"friendly_name\":\"reg_aaa\"},"
            " {\"friendly_name\":\"reg_mmm\"}]");

    names = zb_name_registry_get_names ();
    for (i = 1; i < names->len; i++)
        CHECK (g_strcmp0 (g_ptr_array_index (names, i - 1),
                          g_ptr_array_index (names, i)) <= 0);
    g_ptr_array_unref (names);
}

/* The "changed" signal fires exactly when the set actually grows: on a new
 * name, but not when re-ingesting names already known. */
static guint changed_hits;

static void
on_changed (GObject *reg, gpointer user_data)
{
    (void) reg;
    (void) user_data;
    changed_hits++;
}

static void
case_changed_signal (void)
{
    gulong h = g_signal_connect (zb_name_registry_get_object (),
                                 "changed", G_CALLBACK (on_changed), NULL);

    changed_hits = 0;
    ingest ("[{\"friendly_name\":\"reg_signal_new\"}]");
    CHECK_INT_EQ (changed_hits, 1);          /* new name -> one emission */

    ingest ("[{\"friendly_name\":\"reg_signal_new\"}]");
    CHECK_INT_EQ (changed_hits, 1);          /* nothing new -> no emission */

    g_signal_handler_disconnect (zb_name_registry_get_object (), h);
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-name-registry");

    t_add ("adds_names",             case_adds_names);
    t_add ("dedups",                 case_dedups);
    t_add ("merges_across_publishes",case_merges_across_publishes);
    t_add ("skips_unusable",         case_skips_unusable);
    t_add ("ignores_malformed",      case_ignores_malformed);
    t_add ("sorted",                 case_sorted);
    t_add ("changed_signal",         case_changed_signal);

    return t_run ();
}
