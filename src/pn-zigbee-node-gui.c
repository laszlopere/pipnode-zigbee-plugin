/*
 * Copyright (C) 2026 Laszlo Pere
 *
 * This file is part of pipnode-zigbee-plugin, a plugin for Pipnode.  It
 * is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 3, or (at your option)
 * any later version, as published by the Free Software Foundation.
 *
 * The plugin links the Pipnode host solely through the documented plugin
 * interface and is covered by Pipnode's LICENSE.PLUGIN-EXCEPTION; this
 * file's own license is the GPL, version 3 or later.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; see the GNU General Public License for more
 * details.  You should have received a copy of the license in the file
 * COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-zigbee-node-gui.h"
#include "pn-zigbee-name-registry.h"

#include <gtk/gtk.h>

#include <pn-node.h>

/* ------------------------------------------------------------------ */
/*  The editable friendly-name combo editor                            */
/* ------------------------------------------------------------------ */

/* Fill @combo's dropdown from the current name set, leaving the entry text
 * (the bound property value) untouched -- gtk_combo_box_text_remove_all()
 * clears only the model rows of a with-entry combo, not the entry. */
static void
populate_combo (GtkComboBoxText *combo)
{
    GPtrArray *names = zb_name_registry_get_names ();
    guint      i;

    gtk_combo_box_text_remove_all (combo);
    for (i = 0; i < names->len; i++)
        gtk_combo_box_text_append_text (combo,
                                        g_ptr_array_index (names, i));

    g_ptr_array_unref (names);
}

/* Registry "changed" -> refresh the dropdown of a combo still on screen. */
static void
on_registry_changed (GObject *registry, gpointer combo)
{
    (void) registry;
    populate_combo (GTK_COMBO_BOX_TEXT (combo));
}

/* Coalesce a NULL friendly-name (freshly-dropped, unconfigured node) to the
 * empty string so binding it into GtkEntry:text never trips a NULL warning.
 * The reverse direction uses the default transform: an empty entry writes ""
 * back, which every Zigbee node already normalises to "unconfigured". */
static gboolean
name_to_entry (GBinding     *binding,
               const GValue *from,
               GValue       *to,
               gpointer      user_data)
{
    const gchar *s = g_value_get_string (from);

    (void) binding;
    (void) user_data;
    g_value_set_string (to, s != NULL ? s : "");
    return TRUE;
}

/* #PnNodeClass.build_property_editor override, installed on every Zigbee
 * node type that carries a `friendly-name`.  Turns that one row into an
 * editable entry+dropdown combo backed by the background registry; every
 * other property returns %NULL and falls back to the host default editor. */
static GtkWidget *
friendly_name_editor (PnNode     *self,
                      GParamSpec *pspec,
                      GObject    *target,
                      GtkWindow  *dialog_parent)
{
    GtkWidget *combo;
    GtkWidget *entry;

    (void) self;
    (void) dialog_parent;

    if (g_strcmp0 (pspec->name, "friendly-name") != 0)
        return NULL;

    combo = gtk_combo_box_text_new_with_entry ();
    entry = gtk_bin_get_child (GTK_BIN (combo));

    populate_combo (GTK_COMBO_BOX_TEXT (combo));

    /* Bidirectional bind entry <-> node property; SYNC_CREATE seeds the
     * field from the node's current value.  Selecting a dropdown row sets
     * the entry text (native with-entry behaviour), which flows to the
     * property through this binding; typing a name not in the list works
     * exactly as the old plain entry did. */
    g_object_bind_property_full (target, "friendly-name", entry, "text",
                                 G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE,
                                 name_to_entry, NULL, NULL, NULL);

    /* Repopulate live if names arrive / change while the dialog is open;
     * connect_object ties the handler to the combo so it auto-disconnects
     * when the dialog (and combo) are destroyed. */
    g_signal_connect_object (zb_name_registry_get_object (), "changed",
                             G_CALLBACK (on_registry_changed), combo, 0);

    /* Same widget name the host default editor uses, so functional tests
     * locate the row identically regardless of which path built it. */
    gtk_widget_set_name (combo, "pn-prop-friendly-name");
    return combo;
}

/* ------------------------------------------------------------------ */
/*  Public: install onto the node classes                              */
/* ------------------------------------------------------------------ */

void
zb_zigbee_nodes_gui_install (void)
{
    /* The six logic-tier node types that declare a `friendly-name` string
     * property.  Resolved by name (not the PN_TYPE_* macros) so this GUI
     * module needs no link dependency on the logic module -- exactly how
     * pn_inject_gui_install() attaches the host's icon picker. */
    static const gchar *const TYPES[] = {
        "PnZigbeeSwitch",
        "PnZigbeeRelayCommand",
        "PnZigbeeRelayStatus",
        "PnZigbeeRemote",
        "PnZigbeeButton",
        "PnZigbeeWaterLeak",
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (TYPES); i++)
    {
        GType        t = g_type_from_name (TYPES[i]);
        PnNodeClass *k;

        if (t == 0)
            continue;   /* logic module not loaded -- nothing to hook */

        /* Ref (realising the class) is intentionally never released: node
         * classes live for the whole process, and the vfunc slot must stay
         * set for every dialog the editor ever opens. */
        k = PN_NODE_CLASS (g_type_class_ref (t));
        k->build_property_editor = friendly_name_editor;
    }
}
