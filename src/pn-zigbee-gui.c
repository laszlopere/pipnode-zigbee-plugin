/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 *
 * This file is part of the pipnode-zigbee-plugin, a proprietary plugin
 * for Pipnode.  It links the pipnode host library solely through the
 * documented plugin interface (pn_plugin_gui_init and the public pipnode
 * headers) and is therefore distributed under the additional permission
 * granted by Pipnode's LICENSE.PLUGIN-EXCEPTION, which allows a plugin
 * to be released under any license, including this proprietary one.
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

/* GUI companion module for the Zigbee plugin.
 *
 * The logic .so (pn-zigbee-plugin.c) is GTK-free.  This sibling
 * "-gui.so" links the GTK tier and is auto-loaded by the host right
 * after the logic half (PLUGINS section 17); its pn_plugin_gui_init()
 * advertises a "Zigbee" entry in the editor's Devices menu via the
 * provider registry (pn-device-provider.h).
 *
 * Activating the entry opens a small dialog (the reusable device-dialog
 * shell + form helpers) with a combobox of the available MQTT Broker
 * credential profiles -- the same set the MQTT Source node offers.  When
 * the user picks a broker and presses Apply, we spin up a *hidden* MQTT
 * Source node (pn_mqtt_new(), not placed on any worksheet), point it at
 * that broker, subscribe it to "zigbee2mqtt/#" and count every message
 * it emits in a label on the right.  Closing the dialog releases the
 * node (its dispose joins the mosquitto network thread and sends a clean
 * DISCONNECT), so no connection outlives the dialog.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gmodule.h>
#include <gtk/gtk.h>

#include <pn-plugin.h>
#include <pn-node-factory.h>
#include <pn-device-provider.h>
#include <pn-device-dialog.h>
#include <pn-device-form.h>
#include <pn-device-combo.h>
#include <pn-vault.h>
#include <pn-mqtt.h>
#include <pn-mqtt-profile.h>
#include <pn-message.h>

#include <json-glib/json-glib.h>

/* Qdata keys: the per-dialog state lives on the dialog widget (freed on
 * destroy); the parent window remembers its open dialog so re-activating
 * the menu raises the existing one rather than opening a second. */
#define ZB_DEV_CTX_QDATA    "pn-zigbee-dev-ctx"
#define ZB_DEV_DIALOG_QDATA "pn-zigbee-dev-dialog"

/* The hidden source always watches the whole Zigbee2MQTT tree. */
#define ZB_DEV_SUBSCRIBE_TOPIC "zigbee2mqtt/#"

/* Z2M publishes its full device inventory here, retained, as a JSON
 * array of device records -- delivered right after subscribe and again
 * whenever the device set changes.  We read it to populate the list. */
#define ZB_DEV_DEVICES_TOPIC "zigbee2mqtt/bridge/devices"

typedef struct
{
    /* Reusable dialog frame -- owned by the GtkDialog widget, borrowed
     * here.  Cleared to NULL in the ctx free path before tearing the
     * source down (the shell may be freed as a sibling qdata in an
     * unspecified order). */
    PnDeviceDialog  *shell;

    /* Widgets inside the shell's notebook, borrowed. */
    GtkComboBoxText *broker_combo;
    GtkLabel        *count_label;

    /* The hidden MQTT Source.  NULL until the first Apply; re-created on
     * every Apply.  Owned -- released with g_object_unref. */
    PnMqtt          *source;
    gulong           msg_handler;   /* "message" handler id, 0 if none */
    guint64          count;         /* messages received since last Apply */

    /* Our own deep copy of the most recent bridge/devices inventory (a
     * JSON array of device records).  The payload node handed to the
     * message handler is borrowed and freed when the handler returns, so
     * we keep a copy.  NULL until the first inventory arrives. */
    JsonNode        *devices;
} ZbDevCtx;

/* ------------------------------------------------------------------ */
/*  Hidden source lifecycle                                            */
/* ------------------------------------------------------------------ */

/* Disconnect and release the hidden source, if any.  Safe to call when
 * none is running. */
static void
zb_drop_source (ZbDevCtx *ctx)
{
    if (ctx->source == NULL)
        return;

    if (ctx->msg_handler != 0)
    {
        g_signal_handler_disconnect (ctx->source, ctx->msg_handler);
        ctx->msg_handler = 0;
    }
    /* unref -> dispose joins the mosquitto network thread and sends a
     * clean DISCONNECT, so no callback can fire after this returns. */
    g_clear_object (&ctx->source);
}

/* ------------------------------------------------------------------ */
/*  Device inventory -> left-hand list                                 */
/* ------------------------------------------------------------------ */

/* Borrow a string-valued member off @obj, or NULL when absent / not a
 * string.  Valid only while @obj lives. */
static const gchar *
zb_peek_string (JsonObject *obj, const gchar *key)
{
    JsonNode *n;

    if (obj == NULL || !json_object_has_member (obj, key))
        return NULL;
    n = json_object_get_member (obj, key);
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string (n);
}

/* Build the dim second line for a device row from its Z2M definition
 * ("<vendor> <model> — <description>"), falling back to the device type
 * ("Coordinator", "Router", …) for entries without a definition. */
static gchar *
zb_device_secondary (JsonObject *dev)
{
    JsonObject  *def = NULL;
    const gchar *type;

    if (json_object_has_member (dev, "definition"))
    {
        JsonNode *dn = json_object_get_member (dev, "definition");
        if (dn != NULL && JSON_NODE_HOLDS_OBJECT (dn))
            def = json_node_get_object (dn);
    }

    if (def != NULL)
    {
        GString     *s = g_string_new (NULL);
        const gchar *vendor = zb_peek_string (def, "vendor");
        const gchar *model  = zb_peek_string (def, "model");
        const gchar *desc   = zb_peek_string (def, "description");

        if (vendor != NULL)
            g_string_append (s, vendor);
        if (model != NULL)
        {
            if (s->len > 0)
                g_string_append_c (s, ' ');
            g_string_append (s, model);
        }
        if (desc != NULL)
        {
            if (s->len > 0)
                g_string_append (s, " \xe2\x80\x94 ");   /* em dash */
            g_string_append (s, desc);
        }
        if (s->len > 0)
            return g_string_free (s, FALSE);
        g_string_free (s, TRUE);
    }

    type = zb_peek_string (dev, "type");
    return g_strdup (type != NULL ? type : "Unknown device");
}

/* Parse a bridge/devices publish: store a private copy of the inventory
 * and rebuild the left-hand device list from it.  A malformed payload is
 * ignored so a momentary bad publish does not blank a good list. */
static void
zb_ingest_devices (ZbDevCtx *ctx, PnMessage *message)
{
    JsonNode  *payload = pn_message_get_member (message, "payload");
    JsonArray *arr;
    GPtrArray *rows;
    guint      n, i, kept = 0;

    if (payload == NULL || !JSON_NODE_HOLDS_ARRAY (payload))
        return;

    /* Keep our own deep copy -- the message owns the borrowed node. */
    g_clear_pointer (&ctx->devices, json_node_unref);
    ctx->devices = json_node_copy (payload);

    arr  = json_node_get_array (payload);
    n    = json_array_get_length (arr);
    rows = pn_device_row_array_new ();

    for (i = 0; i < n; i++)
    {
        JsonNode    *elem = json_array_get_element (arr, i);
        JsonObject  *dev;
        const gchar *ieee, *fname;
        gchar       *secondary;

        if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
            continue;
        dev   = json_node_get_object (elem);
        fname = zb_peek_string (dev, "friendly_name");
        ieee  = zb_peek_string (dev, "ieee_address");
        if (fname == NULL && ieee == NULL)
            continue;

        /* id = stable IEEE address (fall back to the name); primary =
         * the friendly name the user knows the device by. */
        secondary = zb_device_secondary (dev);
        g_ptr_array_add (rows, pn_device_row_new (
                ieee  != NULL ? ieee  : fname,
                fname != NULL ? fname : ieee,
                secondary,
                NULL));           /* never disabled -- all are listable */
        g_free (secondary);
        kept++;
    }

    /* The shell copies each row into its list; we still own the array. */
    pn_device_dialog_set_device_rows (ctx->shell, rows);
    g_ptr_array_unref (rows);

    pn_device_dialog_set_statusf (ctx->shell,
                                  "%u device%s reported by Zigbee2MQTT.",
                                  kept, kept == 1 ? "" : "s");
}

/* "message" signal handler.  The base PnMqtt marshals every PUBLISH onto
 * the main thread before emitting, so we can touch GTK directly here. */
static void
zb_on_message (PnMqtt *source, PnMessage *message, gpointer user_data)
{
    ZbDevCtx    *ctx = user_data;
    const gchar *topic;
    gchar       *text;

    (void) source;

    ctx->count++;
    text = g_strdup_printf ("%" G_GUINT64_FORMAT, ctx->count);
    pn_device_form_set_value (ctx->count_label, text);
    g_free (text);

    topic = pn_message_get_topic (message);
    if (g_strcmp0 (topic, ZB_DEV_DEVICES_TOPIC) == 0)
        zb_ingest_devices (ctx, message);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                              */
/* ------------------------------------------------------------------ */

/* (Re)connect the hidden source to the broker currently selected in the
 * combo, from a clean slate: drop any previous session, zero the counter
 * and the device list, then spin up a fresh source.  Shared by the Apply
 * button and the list pane's Reload (the retained bridge/devices inventory
 * re-arrives on reconnect, so Reload genuinely refreshes the list). */
static void
zb_start_source (ZbDevCtx *ctx)
{
    const gchar *id;
    gchar       *label;

    zb_drop_source (ctx);
    ctx->count = 0;
    pn_device_form_set_value (ctx->count_label, "0");
    g_clear_pointer (&ctx->devices, json_node_unref);
    pn_device_dialog_set_device_rows (ctx->shell, NULL);   /* clear list */

    /* The active id is the profile id; "" follows the primary broker.
     * active-id is a GtkComboBox property (not GtkComboBoxText). */
    id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (ctx->broker_combo));
    if (id == NULL)
        id = "";
    label = gtk_combo_box_text_get_active_text (ctx->broker_combo);

    /* Spin up the hidden MQTT Source.  Setting the properties schedules
     * the connect on the next main-loop idle (PnMqtt debounces it). */
    ctx->source = pn_mqtt_new ();
    g_object_set (ctx->source,
                  "broker-profile",  id,
                  "subscribe-topic", ZB_DEV_SUBSCRIBE_TOPIC,
                  NULL);
    ctx->msg_handler = g_signal_connect (ctx->source, "message",
                                         G_CALLBACK (zb_on_message), ctx);

    pn_device_dialog_set_statusf (ctx->shell,
                                  "Connecting via %s and watching %s…",
                                  (label != NULL && *label != '\0')
                                      ? label : "the default broker",
                                  ZB_DEV_SUBSCRIBE_TOPIC);
    g_free (label);
}

static void
zb_on_apply_clicked (GtkButton *button, gpointer user_data)
{
    (void) button;
    zb_start_source (user_data);
}

/* List-pane Reload (right-click): reconnect so the retained inventory is
 * redelivered.  A no-op until the user has applied a broker at least once
 * -- the combo selection alone does not start a connection. */
static void
zb_on_scan (gpointer user_data)
{
    ZbDevCtx *ctx = user_data;

    if (ctx->source != NULL)
        zb_start_source (ctx);
    else
        pn_device_dialog_set_status (
                ctx->shell, "Pick a broker and press Apply to discover devices.");
}

/* A device row was clicked: echo which device to the status bar.  @row is
 * borrowed; its fields come straight from the list we built. */
static void
zb_on_device_selected (const PnDeviceRow *row, gpointer user_data)
{
    ZbDevCtx *ctx = user_data;

    if (row == NULL)
        return;
    pn_device_dialog_set_statusf (ctx->shell, "%s  (%s)",
                                  row->primary != NULL ? row->primary : "device",
                                  row->id != NULL ? row->id : "?");
}

/* ------------------------------------------------------------------ */
/*  Dialog construction                                                */
/* ------------------------------------------------------------------ */

/* Fill the broker combo from the vault: a leading "Default (<primary>)"
 * entry mapping to the empty id, then one row per mqtt-broker profile.
 * Mirrors the host's profile-ref picker (pn-node-dialog.c). */
static void
zb_populate_brokers (ZbDevCtx *ctx)
{
    PnVault   *vault = pn_vault_get_default ();
    PnProfile *primary;
    GList     *profiles, *l;
    gchar     *def_label;

    primary = pn_vault_get_default_profile (vault, PN_PROFILE_TYPE_MQTT_BROKER);
    def_label = (primary != NULL)
        ? g_strdup_printf ("Default (%s)", pn_profile_get_name (primary))
        : g_strdup ("Default (none set)");
    gtk_combo_box_text_append (ctx->broker_combo, "", def_label);
    g_free (def_label);

    profiles = pn_vault_list_profiles (vault, PN_PROFILE_TYPE_MQTT_BROKER);
    for (l = profiles; l != NULL; l = l->next)
    {
        PnProfile *p = l->data;
        gtk_combo_box_text_append (ctx->broker_combo,
                                   pn_profile_get_id (p),
                                   pn_profile_get_name (p));
    }
    g_list_free (profiles);   /* profiles are borrowed from the vault */

    gtk_combo_box_set_active (GTK_COMBO_BOX (ctx->broker_combo), 0);
}

static GtkWidget *
zb_build_dialog (GtkWindow *parent, ZbDevCtx *ctx)
{
    GtkWidget *dialog;
    GtkWidget *inner;
    GtkWidget *tab;
    GtkWidget *grid;
    GtkWidget *combo;
    GtkWidget *apply;
    GtkWidget *cell;
    gint       row = 0;

    ctx->shell = pn_device_dialog_new (parent, "Zigbee Devices",
                                       PN_DEVICE_DIALOG_WITH_DEVICE_LIST);
    dialog = pn_device_dialog_get_dialog (ctx->shell);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 720, 420);

    tab = pn_device_form_new_tab (&inner);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);

    /* "Broker" | combo. */
    cell = pn_device_form_attach_control_row (GTK_GRID (grid), row++, "Broker");
    combo = pn_device_combo_new ();   /* wheel-safe GtkComboBoxText */
    ctx->broker_combo = GTK_COMBO_BOX_TEXT (combo);
    gtk_widget_set_tooltip_text (combo,
            "MQTT Broker credential profile the hidden Zigbee2MQTT "
            "source connects through.  \"Default\" follows the primary "
            "mqtt-broker profile.");
    gtk_box_pack_start (GTK_BOX (cell), combo, TRUE, TRUE, 0);

    /* "Messages received" | live count (the right-side label). */
    ctx->count_label =
        pn_device_form_attach_label_row (GTK_GRID (grid), row++,
                                         "Messages received");
    pn_device_form_set_value (ctx->count_label, "0");

    /* Apply, right-aligned under the value column. */
    apply = gtk_button_new_with_label ("Apply");
    gtk_widget_set_halign (apply, GTK_ALIGN_END);
    gtk_widget_set_margin_top (apply, 4);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (zb_on_apply_clicked), ctx);
    gtk_grid_attach (GTK_GRID (grid), apply, 1, row++, 1, 1);

    pn_device_form_add_section (inner, "MQTT Broker", grid);
    pn_device_dialog_append_page (ctx->shell, tab, "Broker");

    /* Left-hand device list.  We do not auto-scan (there is nothing to
     * scan -- devices arrive over MQTT once a broker is applied); the
     * list is driven by bridge/devices publishes via set_device_rows.
     * Reload (right-click) reconnects so the retained inventory replays. */
    pn_device_dialog_set_list_hints (
            ctx->shell,
            "network-wireless-disabled",
            "No devices yet.",
            "Pick a broker and press Apply.",
            "dialog-question",
            "No devices reported.",
            "Is Zigbee2MQTT running and publishing bridge/devices?");
    pn_device_dialog_set_scan_callback (ctx->shell, zb_on_scan, ctx);
    pn_device_dialog_set_selected_callback (ctx->shell,
                                            zb_on_device_selected, ctx);

    zb_populate_brokers (ctx);
    pn_device_dialog_set_status (ctx->shell,
                                 "Pick an MQTT broker and press Apply.");

    gtk_widget_show_all (gtk_dialog_get_content_area (GTK_DIALOG (dialog)));
    return dialog;
}

/* ------------------------------------------------------------------ */
/*  Lifetime                                                           */
/* ------------------------------------------------------------------ */

static void
zb_dev_ctx_free (gpointer data)
{
    ZbDevCtx *ctx = data;

    /* The shell is owned by the dialog widget and may be torn down as a
     * sibling qdata before or after us; drop our borrowed pointer first
     * so nothing below touches it. */
    ctx->shell = NULL;
    zb_drop_source (ctx);
    g_clear_pointer (&ctx->devices, json_node_unref);
    g_free (ctx);
}

static void
zb_on_dialog_destroyed (gpointer parent, GObject *was_dialog)
{
    (void) was_dialog;
    g_object_set_data (G_OBJECT (parent), ZB_DEV_DIALOG_QDATA, NULL);
}

/* PnDeviceProviderPresentFunc: open the Zigbee devices dialog, or raise
 * the one already open for @parent. */
static void
zb_dialog_present (GtkWindow *parent, gpointer user_data)
{
    GtkWidget *dialog;
    ZbDevCtx  *ctx;

    (void) user_data;
    g_return_if_fail (parent == NULL || GTK_IS_WINDOW (parent));

    if (parent != NULL)
    {
        dialog = g_object_get_data (G_OBJECT (parent), ZB_DEV_DIALOG_QDATA);
        if (dialog != NULL)
        {
            gtk_window_present (GTK_WINDOW (dialog));
            return;
        }
    }

    ctx = g_new0 (ZbDevCtx, 1);
    dialog = zb_build_dialog (parent, ctx);

    /* Ctx dies with the dialog. */
    g_object_set_data_full (G_OBJECT (dialog), ZB_DEV_CTX_QDATA,
                            ctx, zb_dev_ctx_free);

    if (parent != NULL)
    {
        g_object_set_data (G_OBJECT (parent), ZB_DEV_DIALOG_QDATA, dialog);
        g_object_weak_ref (G_OBJECT (dialog),
                           (GWeakNotify) zb_on_dialog_destroyed, parent);
    }

    gtk_window_present (GTK_WINDOW (dialog));
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_gui_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-zigbee-gui",
        .version     = "0.1.0",
        .description = "Zigbee Devices dialog (GUI companion).",
    };

    (void) factory;   /* no per-type vfuncs to install; the menu is all */

    /* "network-wireless" reads as "talk to a radio device" and ships
     * with every common icon theme -- the same icon Meshtastic uses. */
    pn_device_provider_register ("zigbee", "Zigbee", "network-wireless",
                                 zb_dialog_present, NULL, NULL);

    return &info;
}
