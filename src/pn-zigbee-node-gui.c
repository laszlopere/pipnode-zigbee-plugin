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
/*                                                                     */
/*  A GtkComboBox with an entry, backed by a three-column list store    */
/*  (type glyph, friendly name, human-readable type).  The entry -- and */
/*  thus the stored property value -- is just the friendly name; each   */
/*  dropdown row shows a tall magenta FontAwesome type glyph on the     */
/*  left, then two text lines: the name in the normal foreground, the   */
/*  type below it in a smaller, dimmer grey.                            */
/* ------------------------------------------------------------------ */

enum { COL_ICON, COL_NAME, COL_TYPE, N_COLS };

/* The nodes' magenta (PnColor {0.78,0.27,0.60} = rgb 199,69,153); the type
 * glyph is drawn in it so the row ties back to the Zigbee node palette. */
#define ZB_ICON_COLOR "#C74599"

/* A single glyph sized to span the two text lines, vertically centred. */
#define ZB_ICON_FONT  "FontAwesome 18"

/* Map a one-word device category (from the registry) to the FontAwesome glyph
 * shown at the row's left.  Reuses the Zigbee nodes' own glyphs where a node
 * exists (switch/leak/remote), and stock FA4 glyphs otherwise; an unknown or
 * missing category falls back to a generic cube.  All codepoints exist in the
 * classic "FontAwesome" family. */
static const gchar *
category_glyph (const gchar *category)
{
    if (category == NULL)                       return "\xef\x86\xb2";  /* fa-cube          U+F1B2 */
    if (g_strcmp0 (category, "switch")      == 0) return "\xef\x88\x85";/* fa-toggle-on      U+F205 */
    if (g_strcmp0 (category, "plug")        == 0) return "\xef\x87\xa6";/* fa-plug           U+F1E6 */
    if (g_strcmp0 (category, "light")       == 0) return "\xef\x83\xab";/* fa-lightbulb-o    U+F0EB */
    if (g_strcmp0 (category, "leak")        == 0) return "\xef\x81\x83";/* fa-tint           U+F043 */
    if (g_strcmp0 (category, "sensor")      == 0) return "\xef\x8b\x9b";/* fa-microchip      U+F2DB */
    if (g_strcmp0 (category, "remote")      == 0) return "\xef\x89\x9a";/* fa-hand-pointer   U+F25A */
    if (g_strcmp0 (category, "lock")        == 0) return "\xef\x80\xa3";/* fa-lock           U+F023 */
    if (g_strcmp0 (category, "cover")       == 0) return "\xef\x8b\x90";/* fa-window-maximize U+F2D0 */
    if (g_strcmp0 (category, "fan")         == 0) return "\xef\x8b\x9c";/* fa-snowflake-o    U+F2DC */
    if (g_strcmp0 (category, "climate")     == 0) return "\xef\x8b\x89";/* fa-thermometer-half U+F2C9 */
    if (g_strcmp0 (category, "coordinator") == 0) return "\xef\x83\xa8";/* fa-sitemap        U+F0E8 */
    return "\xef\x86\xb2";                       /* fa-cube (device / unknown) U+F1B2 */
}

/* Fill @store from the current registry snapshot: one row per name, with its
 * type glyph and recorded type (or empty).  Clearing + refilling the store
 * leaves the combo's entry text (the bound property value) untouched. */
static void
fill_store (GtkListStore *store)
{
    GPtrArray *names = zb_name_registry_get_names ();
    guint      i;

    gtk_list_store_clear (store);
    for (i = 0; i < names->len; i++)
    {
        const gchar *name = g_ptr_array_index (names, i);
        gchar       *type = zb_name_registry_lookup_type (name);
        gchar       *cat  = zb_name_registry_lookup_category (name);

        gtk_list_store_insert_with_values (store, NULL, -1,
                                           COL_ICON, category_glyph (cat),
                                           COL_NAME, name,
                                           COL_TYPE, type != NULL ? type : "",
                                           -1);
        g_free (type);
        g_free (cat);
    }
    g_ptr_array_unref (names);
}

/* Render a dropdown row on two lines: the friendly name normally, the type
 * beneath it smaller and grey.  A device with no known type shows just the
 * name (no dangling blank second line). */
static void
row_cell_data (GtkCellLayout   *layout,
               GtkCellRenderer *cell,
               GtkTreeModel    *model,
               GtkTreeIter     *iter,
               gpointer         user_data)
{
    gchar *name = NULL;
    gchar *type = NULL;
    gchar *markup;

    (void) layout;
    (void) user_data;

    gtk_tree_model_get (model, iter, COL_NAME, &name, COL_TYPE, &type, -1);

    if (type != NULL && *type != '\0')
        markup = g_markup_printf_escaped (
                "%s\n<small><span foreground=\"#888888\">%s</span></small>",
                name != NULL ? name : "", type);
    else
        markup = g_markup_printf_escaped ("%s", name != NULL ? name : "");

    g_object_set (cell, "markup", markup, NULL);

    g_free (markup);
    g_free (name);
    g_free (type);
}

/* Registry "changed" -> refresh the dropdown of a combo still on screen. */
static void
on_registry_changed (GObject *registry, gpointer combo)
{
    GtkTreeModel *model = gtk_combo_box_get_model (GTK_COMBO_BOX (combo));

    (void) registry;
    if (GTK_IS_LIST_STORE (model))
        fill_store (GTK_LIST_STORE (model));
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
    GtkListStore    *store;
    GtkWidget       *combo;
    GtkWidget       *entry;
    GtkCellRenderer *cell;

    (void) self;
    (void) dialog_parent;

    if (g_strcmp0 (pspec->name, "friendly-name") != 0)
        return NULL;

    store = gtk_list_store_new (N_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                G_TYPE_STRING);
    combo = gtk_combo_box_new_with_model_and_entry (GTK_TREE_MODEL (store));
    g_object_unref (store);   /* combo now holds the only ref */

    /* The entry -- and thus the stored property value -- is the friendly
     * name column; the icon and type columns are display-only. */
    gtk_combo_box_set_entry_text_column (GTK_COMBO_BOX (combo), COL_NAME);
    entry = gtk_bin_get_child (GTK_BIN (combo));

    /* Own the popup rendering: clear any default renderer, then pack the
     * left-hand type-glyph cell followed by the two-line text cell. */
    gtk_cell_layout_clear (GTK_CELL_LAYOUT (combo));

    /* Type glyph: a tall magenta FontAwesome glyph, vertically centred so it
     * spans both text lines.  A plain COL_ICON text attribute -- no data func
     * -- since every row uses the same font/colour. */
    cell = gtk_cell_renderer_text_new ();
    g_object_set (cell,
                  "font",       ZB_ICON_FONT,
                  "foreground", ZB_ICON_COLOR,
                  "yalign",     0.5,
                  "xpad",       4,
                  NULL);
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), cell, FALSE);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo), cell,
                                    "text", COL_ICON, NULL);

    /* Two-line name/type text. */
    cell = gtk_cell_renderer_text_new ();
    g_object_set (cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), cell, TRUE);
    gtk_cell_layout_set_cell_data_func (GTK_CELL_LAYOUT (combo), cell,
                                        row_cell_data, NULL, NULL);

    fill_store (store);

    /* Bidirectional bind entry <-> node property; SYNC_CREATE seeds the
     * field from the node's current value.  Selecting a dropdown row sets
     * the entry text (the name column) which flows to the property through
     * this binding; typing a name not in the list works exactly as the old
     * plain entry did. */
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
