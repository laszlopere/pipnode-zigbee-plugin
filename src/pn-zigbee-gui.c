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

#include <string.h>

#include <gmodule.h>
#include <gtk/gtk.h>

#include <pn-plugin.h>
#include <pn-node-factory.h>
#include <pn-device-provider.h>
#include <pn-device-dialog.h>
#include <pn-device-form.h>
#include <pn-device-combo.h>
#include <pn-device-spin.h>
#include <pn-inline-edit-label.h>
#include <pn-vault.h>
#include <pn-mqtt.h>
#include <pn-mqtt-sink.h>
#include <pn-mqtt-profile.h>
#include <pn-message.h>
#include <pn-node.h>

#include <json-glib/json-glib.h>

/* Qdata keys: the per-dialog state lives on the dialog widget (freed on
 * destroy); the parent window remembers its open dialog so re-activating
 * the menu raises the existing one rather than opening a second. */
#define ZB_DEV_CTX_QDATA    "pn-zigbee-dev-ctx"
#define ZB_DEV_DIALOG_QDATA "pn-zigbee-dev-dialog"

/* The borrowed per-page control list a settings page's Apply button
 * carries, so its handler publishes only that page's controls. */
#define ZB_PAGE_CONTROLS_QDATA "pn-zigbee-page-controls"

/* The owned device-state key a per-control "get" refresh button carries
 * (item j), so its handler knows which property to re-request. */
#define ZB_GET_KEY_QDATA "pn-zigbee-get-key"

/* The owned last-committed description an inline-edit Description label
 * carries, so its ::committed handler skips a no-op re-publish (the signal
 * fires on every Enter / focus-out, even when nothing changed). */
#define ZB_DESC_BASELINE_QDATA "pn-zigbee-desc-baseline"

/* The hidden source always watches the whole Zigbee2MQTT tree. */
#define ZB_DEV_SUBSCRIBE_TOPIC "zigbee2mqtt/#"

/* Common prefix of every Zigbee2MQTT topic.  A bare device-state topic is
 * ZB_DEV_TOPIC_PREFIX "<friendly_name>" with nothing further after it. */
#define ZB_DEV_TOPIC_PREFIX "zigbee2mqtt/"

/* Z2M publishes its full device inventory here, retained, as a JSON
 * array of device records -- delivered right after subscribe and again
 * whenever the device set changes.  We read it to populate the list. */
#define ZB_DEV_DEVICES_TOPIC "zigbee2mqtt/bridge/devices"

/* Device-level settings (friendly name, disabled, retain, definition.options,
 * removal) are set through Z2M's bridge request API, not the per-device
 * "<name>/set" path the live-state controls use.  Requests go to these topics;
 * Z2M acknowledges on the matching bridge/response/device/<cmd> topic, which we
 * surface in the status bar (a successful change also re-publishes
 * bridge/devices, so the list refreshes itself). */
/* Pairing window: publish {"value":true,"time":<seconds>} to open the
 * coordinator's join window, {"value":false} to close it early.  Z2M
 * acknowledges on the response topic with {status, error}; we surface a
 * failure there so a rejected request is not mistaken for an open window. */
#define ZB_DEV_PERMIT_TOPIC          "zigbee2mqtt/bridge/request/permit_join"
#define ZB_DEV_PERMIT_RESPONSE_TOPIC "zigbee2mqtt/bridge/response/permit_join"

/* Length of the pairing window we hold open, in seconds (the requested 5 min). */
#define ZB_JOIN_WINDOW_SECONDS 300

/* The Zigbee permit-join duration is a single octet carried in the ZDO
 * Mgmt_Permit_Joining request, so a single request can open the window for at
 * most 254 seconds (zigbee-herdsman asserts time <= 254; 255 means "forever"
 * and Z2M does not use it).  To honour the longer ZB_JOIN_WINDOW_SECONDS we
 * grant in <=254 s chunks and re-arm before each lapses. */
#define ZB_JOIN_CHUNK_SECONDS  254

/* Re-arm this many seconds before the current grant lapses, so a fresh grant
 * is in place across the seam (covers MQTT + radio round-trip).  Must be < the
 * chunk length. */
#define ZB_JOIN_REARM_LEAD     10

#define ZB_DEV_RENAME_TOPIC   "zigbee2mqtt/bridge/request/device/rename"
#define ZB_DEV_OPTIONS_TOPIC  "zigbee2mqtt/bridge/request/device/options"
#define ZB_DEV_REMOVE_TOPIC   "zigbee2mqtt/bridge/request/device/remove"
#define ZB_DEV_RESPONSE_PREFIX "zigbee2mqtt/bridge/response/device/"

/* What kind of widget a built settings control is, so the value-seeding
 * pass (item f) and, later, write-back (item g) can read/write it without
 * re-deriving the expose type. */
typedef enum
{
    ZB_CTL_READONLY,   /* a value label (pn_device_form_attach_label_row) */
    ZB_CTL_BINARY,     /* a GtkSwitch */
    ZB_CTL_NUMERIC,    /* a PnDeviceSpin */
    ZB_CTL_ENUM,       /* a PnDeviceCombo */
    ZB_CTL_TEXT        /* a GtkEntry */
} ZbCtlKind;

/* Which Z2M channel a control's changed value is published to on Apply.  The
 * live-state exposes (TODO 9) all use ZB_TGT_SET; the device-level settings
 * (this change) route through the bridge request API instead. */
typedef enum
{
    ZB_TGT_SET,        /* zigbee2mqtt/<name>/set, key=value (default) */
    ZB_TGT_OPTIONS,    /* device/options, into {id, options:{key:value}} */
    ZB_TGT_RENAME      /* device/rename, the value is the new friendly name */
} ZbTarget;

/* One built settings control, bound to the device-state JSON key it shows.
 * Tracked per-dialog and cleared in lockstep with the device pages (the
 * widget itself is owned by its notebook page; this record only borrows
 * it).  value_on/value_off carry a binary expose's string mapping so the
 * seed pass can turn "ON"/"OFF" (or a JSON bool) into a switch state. */
typedef struct
{
    gchar      *key;        /* resolved device-state property key (owned) */
    ZbCtlKind   kind;
    ZbTarget    target;     /* which Z2M channel Apply publishes it to */
    GtkWidget  *widget;     /* the control or value label (borrowed) */
    gchar      *value_on;   /* binary mapping, owned; NULL -> JSON bool */
    gchar      *value_off;

    /* The control's value as of the last seed / Apply (owned string, NULL
     * for read-only rows), so Apply publishes only the properties the user
     * actually changed.  Updated on seed and on a successful Apply. */
    gchar      *baseline;
} ZbControl;

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

    /* The hidden MQTT Sink, created/destroyed alongside the source against
     * the same broker profile.  The write-back channel: a settings page's
     * Apply hands it a PnMessage (topic zigbee2mqtt/<name>/set, structured
     * data.payload) via pn_node_receive_message and it PUBLISHes it.  NULL
     * until the first Apply; owned -- released with g_object_unref. */
    PnMqttSink      *sink;

    /* Our own deep copy of the most recent bridge/devices inventory (a
     * JSON array of device records).  The payload node handed to the
     * message handler is borrowed and freed when the handler returns, so
     * we keep a copy.  NULL until the first inventory arrives. */
    JsonNode        *devices;

    /* Live per-device state: friendly_name (owned gchar*) -> a deep copy
     * of the latest retained zigbee2mqtt/<friendly_name> payload (owned
     * JsonNode object, freed with json_node_unref).  Refreshed on every
     * bare device-state publish; the seed source for the settings
     * controls. */
    GHashTable      *state;

    /* friendly_name of the device whose settings pages are currently
     * shown on the right (owned), or NULL when none is selected. */
    gchar           *selected_name;

    /* The dynamically-built device pages currently in the notebook
     * (borrowed GtkWidget*; the notebook owns them).  Tracked so a
     * re-select or dialog close can drop exactly those, keeping the
     * static "Broker" page.  Never NULL once the dialog is built. */
    GPtrArray       *device_pages;

    /* The settings controls built into those pages (owned ZbControl*; the
     * array frees them).  Borrows each control widget from its page, so it
     * is cleared together with device_pages.  The seed/write-back passes
     * iterate it.  Never NULL once the dialog is built. */
    GPtrArray       *controls;

    /* Pairing ("join window") UI.  join_start is the Broker page's "open join
     * window" button (borrowed; the page owns it), insensitive until a broker
     * is applied; join_spinner is the marker beside it, spun while a window is
     * open.  The rest belong to the modal countdown dialog that runs while the
     * window is open: the dialog and the two widgets it updates each tick, the
     * per-second timeout source (0 when none), the window length and seconds
     * left, the inventory size captured when it opened and the count of devices
     * that have joined since.  join_dialog is NULL when no window is open. */
    GtkWidget       *join_start;
    GtkWidget       *join_spinner;
    GtkWidget       *join_dialog;
    GtkWidget       *join_progress;
    GtkWidget       *join_label;
    guint            join_tick_id;
    guint            join_total;
    guint            join_left;
    /* Seconds left on the coordinator's *current* permit-join grant (each is
     * <= ZB_JOIN_CHUNK_SECONDS).  The tick re-arms a fresh grant as this nears
     * zero while join_left still has window to cover. */
    guint            join_grant_left;
    guint            join_base_count;
    guint            join_joined;
} ZbDevCtx;

static void
zb_control_free (gpointer data)
{
    ZbControl *c = data;

    g_free (c->key);
    g_free (c->value_on);
    g_free (c->value_off);
    g_free (c->baseline);
    g_free (c);
}

/* Defined in the settings-pages section below; the message handler
 * (zb_capture_state) calls it to repaint the shown controls from a
 * confirmed re-publish (item h). */
static void zb_seed_controls (ZbDevCtx *ctx);

/* Defined in the pairing section below; the inventory handler
 * (zb_ingest_devices) calls it to refresh the open join dialog's joined
 * count from the freshly-arrived device list. */
static void zb_join_on_inventory (ZbDevCtx *ctx);

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

    /* The write-back sink rides the same lifecycle as the source -- its
     * dispose likewise joins its own network thread and disconnects. */
    g_clear_object (&ctx->sink);

    /* Pairing needs a connected sink; with none, the join button is dead.
     * (Guarded on shell: the ctx-free path clears it before dropping us,
     * by when the button widget may already be gone.) */
    if (ctx->shell != NULL && ctx->join_start != NULL)
        gtk_widget_set_sensitive (ctx->join_start, FALSE);
}

/* Drop the dynamically-built device pages from the notebook, keeping the
 * static "Broker" page, and forget them.  Safe to call when none are
 * built.  Only touches the notebook while the shell is alive: in the
 * dialog-teardown path (ctx->shell cleared first) the notebook is already
 * being destroyed, so we just forget our borrowed pointers. */
static void
zb_clear_device_pages (ZbDevCtx *ctx)
{
    if (ctx->device_pages == NULL)
        return;

    if (ctx->shell != NULL)
    {
        GtkNotebook *nb = pn_device_dialog_get_notebook (ctx->shell);
        guint        i;

        for (i = 0; i < ctx->device_pages->len; i++)
        {
            GtkWidget *page = g_ptr_array_index (ctx->device_pages, i);
            gint       pos  = gtk_notebook_page_num (nb, page);

            if (pos >= 0)
                gtk_notebook_remove_page (nb, pos);
        }
    }

    /* Removing a page destroys its control widgets; drop our borrowed
     * bindings to them in lockstep so none dangle. */
    g_ptr_array_set_size (ctx->device_pages, 0);
    if (ctx->controls != NULL)
        g_ptr_array_set_size (ctx->controls, 0);
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

/* Borrow the device record whose IEEE address (or, lacking one, friendly
 * name) equals @id -- the stable key the list rows were built with -- from
 * the stored inventory.  NULL when absent.  Valid only while ctx->devices
 * is unchanged. */
static JsonObject *
zb_find_device (ZbDevCtx *ctx, const gchar *id)
{
    JsonArray *arr;
    guint      n, i;

    if (ctx->devices == NULL || id == NULL ||
        !JSON_NODE_HOLDS_ARRAY (ctx->devices))
        return NULL;

    arr = json_node_get_array (ctx->devices);
    n   = json_array_get_length (arr);
    for (i = 0; i < n; i++)
    {
        JsonNode    *elem = json_array_get_element (arr, i);
        JsonObject  *dev;
        const gchar *ieee, *fname;

        if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
            continue;
        dev   = json_node_get_object (elem);
        ieee  = zb_peek_string (dev, "ieee_address");
        fname = zb_peek_string (dev, "friendly_name");

        if (g_strcmp0 (ieee, id) == 0 ||
            (ieee == NULL && g_strcmp0 (fname, id) == 0))
            return dev;
    }
    return NULL;
}

/* Borrow the @key array ("exposes" / "options") off @dev's definition, or
 * NULL when the device has no definition or no such array (e.g. the
 * Coordinator).  Valid only while @dev lives. */
static JsonArray *
zb_definition_array (JsonObject *dev, const gchar *key)
{
    JsonNode   *dn, *kn;
    JsonObject *def;

    if (!json_object_has_member (dev, "definition"))
        return NULL;
    dn = json_object_get_member (dev, "definition");
    if (dn == NULL || !JSON_NODE_HOLDS_OBJECT (dn))
        return NULL;
    def = json_node_get_object (dn);
    if (!json_object_has_member (def, key))
        return NULL;
    kn = json_object_get_member (def, key);
    if (kn == NULL || !JSON_NODE_HOLDS_ARRAY (kn))
        return NULL;
    return json_node_get_array (kn);
}

/* Capture a bare device-state publish (topic zigbee2mqtt/<friendly_name>,
 * no further '/').  Stores a deep copy of the payload object in the
 * friendly_name -> state map, replacing any prior value -- the
 * current-value source the settings controls seed from.  Topics with a
 * deeper path (bridge/*, <name>/set, <name>/availability, …) are not
 * device state and are ignored. */
static void
zb_capture_state (ZbDevCtx *ctx, const gchar *topic, PnMessage *message)
{
    const gchar *name = topic + strlen (ZB_DEV_TOPIC_PREFIX);
    JsonNode    *payload;

    if (*name == '\0' || strchr (name, '/') != NULL)
        return;

    payload = pn_message_get_member (message, "payload");
    if (payload == NULL || !JSON_NODE_HOLDS_OBJECT (payload))
        return;

    g_hash_table_insert (ctx->state, g_strdup (name), json_node_copy (payload));

    /* (h) When this is the device on screen, repaint its controls from the
     * just-captured state so the UI reflects what the device confirmed
     * (e.g. a value it clamped after an Apply) rather than what was typed.
     * zb_seed_controls reads the same ctx->state entry and re-anchors each
     * painted control's baseline. */
    if (ctx->selected_name != NULL && g_strcmp0 (name, ctx->selected_name) == 0)
        zb_seed_controls (ctx);
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

    /* A device joining mid-window republishes the inventory; refresh the
     * open join dialog's joined count off this fresh list. */
    zb_join_on_inventory (ctx);
}

/* Surface a bridge/response/device/<cmd> acknowledgement in the status bar.
 * Z2M replies here to our rename/options/remove requests with
 * {status: "ok"|"error", error, data}; a successful change also re-publishes
 * bridge/devices, so the list refreshes itself -- we only report the outcome
 * (and a failure's reason, which the user otherwise would not see). */
static void
zb_report_response (ZbDevCtx *ctx, const gchar *topic, PnMessage *message)
{
    const gchar *cmd     = topic + strlen (ZB_DEV_RESPONSE_PREFIX);
    JsonNode    *payload = pn_message_get_member (message, "payload");
    JsonObject  *obj;
    const gchar *status, *error;

    if (payload == NULL || !JSON_NODE_HOLDS_OBJECT (payload))
        return;
    obj    = json_node_get_object (payload);
    status = zb_peek_string (obj, "status");
    error  = zb_peek_string (obj, "error");

    if (g_strcmp0 (status, "error") == 0)
        pn_device_dialog_set_statusf (ctx->shell, "Device %s failed: %s",
                                      cmd,
                                      error != NULL ? error : "unknown error");
    else
        pn_device_dialog_set_statusf (ctx->shell, "Device %s: ok.", cmd);
}

/* Surface the bridge/response/permit_join acknowledgement.  On an error (e.g.
 * a duration the coordinator rejects) Z2M never opens the window, so a silent
 * countdown would be a lie -- report the reason in the status bar. */
static void
zb_report_permit_response (ZbDevCtx *ctx, PnMessage *message)
{
    JsonNode    *payload = pn_message_get_member (message, "payload");
    JsonObject  *obj;
    const gchar *status, *error;

    if (payload == NULL || !JSON_NODE_HOLDS_OBJECT (payload))
        return;
    obj    = json_node_get_object (payload);
    status = zb_peek_string (obj, "status");
    error  = zb_peek_string (obj, "error");

    if (g_strcmp0 (status, "error") == 0)
        pn_device_dialog_set_statusf (
                ctx->shell, "Join window request failed: %s",
                error != NULL ? error : "unknown error");
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
    else if (g_strcmp0 (topic, ZB_DEV_PERMIT_RESPONSE_TOPIC) == 0)
        zb_report_permit_response (ctx, message);
    else if (topic != NULL && g_str_has_prefix (topic, ZB_DEV_RESPONSE_PREFIX))
        zb_report_response (ctx, topic, message);
    else if (topic != NULL && g_str_has_prefix (topic, ZB_DEV_TOPIC_PREFIX))
        zb_capture_state (ctx, topic, message);
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

    /* The inventory and live state replay on reconnect; drop the stale
     * selection, its pages and the per-device state so nothing carries
     * over from the previous broker/session. */
    g_clear_pointer (&ctx->selected_name, g_free);
    g_hash_table_remove_all (ctx->state);
    zb_clear_device_pages (ctx);

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

    /* The write-back sink shares the broker profile.  It publishes each
     * inbound message's data.payload to its envelope topic, so a settings
     * Apply just sets the topic + payload (zb_publish_changes).  Set
     * retain FALSE: a Z2M "<name>/set" command must not be retained, or
     * the broker would replay it to Z2M on every reconnect/restart. */
    ctx->sink = pn_mqtt_sink_new ();
    g_object_set (ctx->sink,
                  "broker-profile", id,
                  "retain",         FALSE,
                  NULL);

    /* A sink is now connected: the pairing section can open a join window. */
    if (ctx->join_start != NULL)
        gtk_widget_set_sensitive (ctx->join_start, TRUE);

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

/* ------------------------------------------------------------------ */
/*  Exposes tree -> settings pages (items d, e, f)                      */
/* ------------------------------------------------------------------ */

/* Borrow the @key array member off @obj, or NULL when absent / not an
 * array.  Valid only while @obj lives. */
static JsonArray *
zb_object_array (JsonObject *obj, const gchar *key)
{
    JsonNode *n;

    if (obj == NULL || !json_object_has_member (obj, key))
        return NULL;
    n = json_object_get_member (obj, key);
    if (n == NULL || !JSON_NODE_HOLDS_ARRAY (n))
        return NULL;
    return json_node_get_array (n);
}

/* A numeric member of @obj as a double, or @fallback when absent / not a
 * number.  Z2M sends value_min/value_max/value_step/access as JSON
 * numbers; an integer arrives as G_TYPE_INT64. */
static gdouble
zb_peek_double (JsonObject *obj, const gchar *key, gdouble fallback)
{
    JsonNode *n;
    GType     t;

    if (obj == NULL || !json_object_has_member (obj, key))
        return fallback;
    n = json_object_get_member (obj, key);
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
        return fallback;
    t = json_node_get_value_type (n);
    if (t == G_TYPE_INT64)
        return (gdouble) json_node_get_int (n);
    if (t == G_TYPE_DOUBLE)
        return json_node_get_double (n);
    return fallback;
}

/* The Z2M access bitmap of an expose (1=published, 2=settable,
 * 4=gettable), or 0 when absent. */
static gint
zb_expose_access (JsonObject *exp)
{
    return (gint) zb_peek_double (exp, "access", 0.0);
}

/* The human label for an expose: label, then name, then property, then
 * type, then a generic fallback.  Borrowed -- valid while @exp lives. */
static const gchar *
zb_expose_label (JsonObject *exp)
{
    const gchar *s;

    if ((s = zb_peek_string (exp, "label"))    != NULL) return s;
    if ((s = zb_peek_string (exp, "name"))     != NULL) return s;
    if ((s = zb_peek_string (exp, "property")) != NULL) return s;
    if ((s = zb_peek_string (exp, "type"))     != NULL) return s;
    return "Setting";
}

/* The device-state JSON key an expose maps to (owned), or NULL when it
 * has neither a property nor a name.  Z2M usually already bakes the
 * endpoint into `property` (a two-gang switch exposes property
 * "state_left" with endpoint "left"), so we only append "_<endpoint>"
 * when it is not already there -- never doubling it into "state_left_left". */
static gchar *
zb_expose_key (JsonObject *exp)
{
    const gchar *prop = zb_peek_string (exp, "property");
    const gchar *name = zb_peek_string (exp, "name");
    const gchar *ep   = zb_peek_string (exp, "endpoint");
    const gchar *base = (prop != NULL) ? prop : name;

    if (base == NULL)
        return NULL;
    if (ep != NULL && *ep != '\0')
    {
        gchar    *suffix = g_strconcat ("_", ep, NULL);
        gboolean  has    = g_str_has_suffix (base, suffix);
        gchar    *key    = has ? g_strdup (base)
                               : g_strconcat (base, "_", ep, NULL);
        g_free (suffix);
        return key;
    }
    return g_strdup (base);
}

/* A JSON scalar (or, failing that, the compact JSON text) as a display
 * string (owned) -- the read-only value rows, enum-combo ids and text
 * entries seed through this. */
static gchar *
zb_json_to_display (JsonNode *node)
{
    if (node != NULL && JSON_NODE_HOLDS_VALUE (node))
    {
        GType t = json_node_get_value_type (node);

        if (t == G_TYPE_STRING)
            return g_strdup (json_node_get_string (node));
        if (t == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (node) ? "true" : "false");
        if (t == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (node));
        if (t == G_TYPE_DOUBLE)
            return g_strdup_printf ("%g", json_node_get_double (node));
    }
    if (node == NULL || JSON_NODE_HOLDS_NULL (node))
        return g_strdup ("");
    return json_to_string (node, FALSE);   /* object / array -> compact */
}

/* The @key member of @obj as a display string (owned), or NULL when the
 * member is absent.  Used for the read-only identity / options rows. */
static gchar *
zb_peek_display (JsonObject *obj, const gchar *key)
{
    if (obj == NULL || !json_object_has_member (obj, key))
        return NULL;
    return zb_json_to_display (json_object_get_member (obj, key));
}

/* A JSON scalar as a double, for seeding a numeric spin.  TRUE on
 * success. */
static gboolean
zb_node_get_double (JsonNode *node, gdouble *out)
{
    GType t;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;
    t = json_node_get_value_type (node);
    if (t == G_TYPE_INT64)   { *out = (gdouble) json_node_get_int (node);     return TRUE; }
    if (t == G_TYPE_DOUBLE)  { *out = json_node_get_double (node);            return TRUE; }
    if (t == G_TYPE_BOOLEAN) { *out = json_node_get_boolean (node) ? 1 : 0;   return TRUE; }
    return FALSE;
}

/* Is a binary expose's value "on", given its value_on mapping?  Accepts a
 * JSON bool, the value_on string (default "ON"/"true" when unmapped) or a
 * non-zero number. */
static gboolean
zb_binary_is_on (JsonNode *node, const gchar *value_on)
{
    GType t;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;
    t = json_node_get_value_type (node);
    if (t == G_TYPE_BOOLEAN)
        return json_node_get_boolean (node);
    if (t == G_TYPE_STRING)
    {
        const gchar *s = json_node_get_string (node);
        if (s == NULL)
            return FALSE;
        if (value_on != NULL)
            return g_strcmp0 (s, value_on) == 0;
        return g_ascii_strcasecmp (s, "ON")   == 0 ||
               g_ascii_strcasecmp (s, "true") == 0;
    }
    if (t == G_TYPE_INT64)
        return json_node_get_int (node) != 0;
    if (t == G_TYPE_DOUBLE)
        return json_node_get_double (node) != 0.0;
    return FALSE;
}

/* Derive a spin button's range from a numeric expose, with permissive
 * fallbacks for an expose that omits the bounds. */
static void
zb_numeric_range (JsonObject *exp, gdouble *min, gdouble *max, gdouble *step)
{
    *min  = zb_peek_double (exp, "value_min",  -1000000.0);
    *max  = zb_peek_double (exp, "value_max",   1000000.0);
    *step = zb_peek_double (exp, "value_step",  1.0);
    if (*step <= 0.0)
        *step = 1.0;
    if (*max <= *min)
    {
        *min = -1000000.0;
        *max =  1000000.0;
    }
}

/* Paint one control's widget from a JSON value, by the control's kind.  The
 * shared painter for the live-state seed (item f/h) and the build-time
 * value_default seed of a device-option control.  Does not touch the baseline
 * -- the caller anchors it after. */
static void
zb_seed_control_node (ZbControl *c, JsonNode *v)
{
    if (v == NULL)
        return;

    switch (c->kind)
    {
    case ZB_CTL_BINARY:
        gtk_switch_set_active (GTK_SWITCH (c->widget),
                               zb_binary_is_on (v, c->value_on));
        break;
    case ZB_CTL_NUMERIC:
        {
            gdouble d;
            if (zb_node_get_double (v, &d))
                gtk_spin_button_set_value (GTK_SPIN_BUTTON (c->widget), d);
        }
        break;
    case ZB_CTL_ENUM:
        {
            gchar *s = zb_json_to_display (v);
            gtk_combo_box_set_active_id (GTK_COMBO_BOX (c->widget), s);
            g_free (s);
        }
        break;
    case ZB_CTL_TEXT:
        {
            gchar *s = zb_json_to_display (v);
            gtk_entry_set_text (GTK_ENTRY (c->widget), s);
            g_free (s);
        }
        break;
    case ZB_CTL_READONLY:
        {
            gchar *s = zb_json_to_display (v);
            pn_device_form_set_value (GTK_LABEL (c->widget), s);
            g_free (s);
        }
        break;
    }
}

/* Record a built control bound to @key for the seed / write-back passes.
 * Takes ownership of @key, @value_on and @value_off.  Returns the tracked
 * control (owned by ctx->controls) so the caller can also file it under a
 * page. */
static ZbControl *
zb_track_control (ZbDevCtx *ctx, gchar *key, ZbCtlKind kind, ZbTarget target,
                  GtkWidget *widget, gchar *value_on, gchar *value_off)
{
    ZbControl *c = g_new0 (ZbControl, 1);

    c->key       = key;
    c->kind      = kind;
    c->target    = target;
    c->widget    = widget;
    c->value_on  = value_on;
    c->value_off = value_off;
    g_ptr_array_add (ctx->controls, c);
    return c;
}

/* The current value of an editable control as a stable string, for change
 * detection against its baseline.  NULL for a read-only row (never in a
 * page's settable list). */
static gchar *
zb_control_read_string (ZbControl *c)
{
    switch (c->kind)
    {
    case ZB_CTL_BINARY:
        return g_strdup (gtk_switch_get_active (GTK_SWITCH (c->widget))
                             ? "1" : "0");
    case ZB_CTL_NUMERIC:
        {
            GtkSpinButton *sp = GTK_SPIN_BUTTON (c->widget);
            gint           digits = gtk_spin_button_get_digits (sp);
            if (digits == 0)
                return g_strdup_printf ("%d",
                                        gtk_spin_button_get_value_as_int (sp));
            return g_strdup_printf ("%.*f", digits,
                                    gtk_spin_button_get_value (sp));
        }
    case ZB_CTL_ENUM:
        {
            const gchar *id =
                gtk_combo_box_get_active_id (GTK_COMBO_BOX (c->widget));
            return g_strdup (id != NULL ? id : "");
        }
    case ZB_CTL_TEXT:
        return g_strdup (gtk_entry_get_text (GTK_ENTRY (c->widget)));
    case ZB_CTL_READONLY:
        break;
    }
    return NULL;
}

/* Add @c's current value to the set-payload object @obj under its key,
 * shaped the way Z2M's "<name>/set" endpoint expects (a binary's
 * value_on/value_off string, or a JSON bool when unmapped; a numeric as
 * int or double per the spin's digits; an enum's selected id; raw text). */
static void
zb_control_set_member (JsonObject *obj, ZbControl *c)
{
    switch (c->kind)
    {
    case ZB_CTL_BINARY:
        {
            gboolean on = gtk_switch_get_active (GTK_SWITCH (c->widget));
            if (c->value_on != NULL)
                json_object_set_string_member (
                        obj, c->key,
                        on ? c->value_on
                           : (c->value_off != NULL ? c->value_off : "OFF"));
            else
                json_object_set_boolean_member (obj, c->key, on);
        }
        break;
    case ZB_CTL_NUMERIC:
        {
            GtkSpinButton *sp = GTK_SPIN_BUTTON (c->widget);
            if (gtk_spin_button_get_digits (sp) == 0)
                json_object_set_int_member (
                        obj, c->key, gtk_spin_button_get_value_as_int (sp));
            else
                json_object_set_double_member (
                        obj, c->key, gtk_spin_button_get_value (sp));
        }
        break;
    case ZB_CTL_ENUM:
        {
            const gchar *id =
                gtk_combo_box_get_active_id (GTK_COMBO_BOX (c->widget));
            if (id != NULL)
                json_object_set_string_member (obj, c->key, id);
        }
        break;
    case ZB_CTL_TEXT:
        json_object_set_string_member (obj, c->key,
                                       gtk_entry_get_text (GTK_ENTRY (c->widget)));
        break;
    case ZB_CTL_READONLY:
        break;
    }
}

/* Publish @obj (an owned JsonObject, consumed here) as the data.payload of a
 * message to @topic through the hidden sink.  The single MQTT publish path for
 * every write the dialog makes -- "<name>/set", "<name>/get" and the bridge
 * requests (device/options, device/rename, device/remove).  A no-op when no
 * sink is connected (the caller has already reported that case). */
static void
zb_publish_to (ZbDevCtx *ctx, const gchar *topic, JsonObject *obj)
{
    JsonNode  *node;
    PnMessage *msg;

    if (ctx->sink == NULL)
    {
        json_object_unref (obj);
        return;
    }
    msg  = pn_message_new (NULL, topic);
    node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (node, obj);              /* transfers obj */
    pn_message_set_member (msg, "payload", node);   /* transfers node */
    pn_node_receive_message (PN_NODE (ctx->sink), msg);
    g_object_unref (msg);
}

/* (g) Collect the controls on @page_controls whose value changed since the
 * last seed/Apply and publish them, each on the channel its target names: live
 * state to zigbee2mqtt/<name>/set, device options to the bridge device/options
 * request, and the friendly name to the device/rename request.  Order matters:
 * a rename changes the name the other two address, so it goes last.  The
 * applied values are adopted as the new baseline so a second press resends
 * nothing. */
static void
zb_publish_changes (ZbDevCtx *ctx, GPtrArray *page_controls)
{
    JsonObject *setobj, *optobj;
    gchar      *rename_to = NULL;       /* owned new friendly name, or NULL */
    guint       i, changed = 0;

    if (ctx->sink == NULL)
    {
        pn_device_dialog_set_status (
                ctx->shell, "Pick a broker and press Apply before writing.");
        return;
    }
    if (ctx->selected_name == NULL)
        return;

    setobj = json_object_new ();
    optobj = json_object_new ();
    for (i = 0; i < page_controls->len; i++)
    {
        ZbControl *c   = g_ptr_array_index (page_controls, i);
        gchar     *cur = zb_control_read_string (c);

        if (c->baseline != NULL && g_strcmp0 (cur, c->baseline) == 0)
        {
            g_free (cur);
            continue;                   /* unchanged -- nothing to publish */
        }

        if (c->target == ZB_TGT_RENAME)
        {
            if (cur == NULL || *cur == '\0')
            {
                g_free (cur);
                continue;               /* never rename to an empty name */
            }
            g_free (rename_to);
            rename_to = g_strdup (cur);  /* the entry text is the new name */
        }
        else if (c->target == ZB_TGT_OPTIONS)
            zb_control_set_member (optobj, c);
        else
            zb_control_set_member (setobj, c);

        g_free (c->baseline);
        c->baseline = cur;              /* adopt the applied value */
        changed++;
    }

    if (changed == 0)
    {
        json_object_unref (setobj);
        json_object_unref (optobj);
        pn_device_dialog_set_statusf (ctx->shell, "%s — no changes to apply.",
                                      ctx->selected_name);
        return;
    }

    /* SET: zigbee2mqtt/<name>/set { key: value, … } */
    if (json_object_get_size (setobj) > 0)
    {
        gchar *topic = g_strdup_printf ("%s%s/set", ZB_DEV_TOPIC_PREFIX,
                                        ctx->selected_name);
        zb_publish_to (ctx, topic, setobj);    /* consumes setobj */
        g_free (topic);
    }
    else
        json_object_unref (setobj);

    /* OPTIONS: device/options { id: <name>, options: { key: value, … } } --
     * Z2M merges into the device's existing options, so only the changed keys
     * are sent and untouched (unseeded) ones are preserved. */
    if (json_object_get_size (optobj) > 0)
    {
        JsonObject *req = json_object_new ();
        json_object_set_string_member (req, "id", ctx->selected_name);
        json_object_set_object_member (req, "options", optobj);  /* transfers */
        zb_publish_to (ctx, ZB_DEV_OPTIONS_TOPIC, req);          /* consumes */
    }
    else
        json_object_unref (optobj);

    /* RENAME last: device/rename { from: <old>, to: <new> }.  Track the new
     * name locally so subsequent get/set/options address the device correctly;
     * Z2M re-publishes bridge/devices on success, which refreshes the list. */
    if (rename_to != NULL && g_strcmp0 (rename_to, ctx->selected_name) != 0)
    {
        JsonObject *req = json_object_new ();
        json_object_set_string_member (req, "from", ctx->selected_name);
        json_object_set_string_member (req, "to",   rename_to);
        zb_publish_to (ctx, ZB_DEV_RENAME_TOPIC, req);           /* consumes */

        g_free (ctx->selected_name);
        ctx->selected_name = g_strdup (rename_to);
    }
    g_free (rename_to);

    pn_device_dialog_set_statusf (ctx->shell, "Applied %u change%s to %s.",
                                  changed, changed == 1 ? "" : "s",
                                  ctx->selected_name);
}

static void
zb_on_page_apply_clicked (GtkButton *button, gpointer user_data)
{
    ZbDevCtx  *ctx      = user_data;
    GPtrArray *controls = g_object_get_data (G_OBJECT (button),
                                             ZB_PAGE_CONTROLS_QDATA);

    if (controls != NULL)
        zb_publish_changes (ctx, controls);
}

/* Add a right-aligned Apply button to @inner wired to publish @page_controls
 * (a borrowed-element array, transferred to the button's qdata so it dies
 * with the page).  When the page has no settable control, frees the array
 * and adds nothing. */
static void
zb_add_page_apply (ZbDevCtx *ctx, GtkWidget *inner, GPtrArray *page_controls)
{
    GtkWidget *apply_box;
    GtkWidget *apply;

    if (page_controls->len == 0)
    {
        g_ptr_array_unref (page_controls);
        return;
    }

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top (apply_box, 8);
    apply = gtk_button_new_with_label ("Apply");
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (inner), apply_box, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (apply), ZB_PAGE_CONTROLS_QDATA,
                            page_controls, (GDestroyNotify) g_ptr_array_unref);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (zb_on_page_apply_clicked), ctx);
}

/* (j) Publish a Z2M "<name>/get" request for a single property, asking the
 * device to re-report its current value.  The payload is { "<key>": "" } --
 * the shape Z2M's get endpoint expects (the value is ignored).  The device's
 * reply arrives as a bare state publish and repaints the control through
 * zb_capture_state -> zb_seed_controls (item h), so there is nothing to wait
 * on here. */
static void
zb_publish_get (ZbDevCtx *ctx, const gchar *key)
{
    JsonObject *obj;
    gchar      *topic;

    if (ctx->sink == NULL)
    {
        pn_device_dialog_set_status (
                ctx->shell, "Pick a broker and press Apply before refreshing.");
        return;
    }
    if (ctx->selected_name == NULL || key == NULL)
        return;

    obj = json_object_new ();
    json_object_set_string_member (obj, key, "");

    topic = g_strdup_printf ("%s%s/get", ZB_DEV_TOPIC_PREFIX,
                             ctx->selected_name);
    zb_publish_to (ctx, topic, obj);                /* consumes obj */
    g_free (topic);

    pn_device_dialog_set_statusf (ctx->shell, "Requested %s from %s…",
                                  key, ctx->selected_name);
}

static void
zb_on_get_clicked (GtkButton *button, gpointer user_data)
{
    const gchar *key = g_object_get_data (G_OBJECT (button), ZB_GET_KEY_QDATA);

    zb_publish_get (user_data, key);
}

/* (j) A small flat refresh button that re-requests @key from the device.
 * Carries its own owned copy of @key (freed with the button) so it does not
 * depend on the tracked control outliving it. */
static GtkWidget *
zb_make_get_button (ZbDevCtx *ctx, const gchar *key)
{
    GtkWidget *btn = gtk_button_new_from_icon_name ("view-refresh",
                                                    GTK_ICON_SIZE_BUTTON);

    gtk_button_set_relief (GTK_BUTTON (btn), GTK_RELIEF_NONE);
    gtk_widget_set_valign  (btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (btn, "Refresh this value from the device");
    g_object_set_data_full (G_OBJECT (btn), ZB_GET_KEY_QDATA,
                            g_strdup (key), g_free);
    g_signal_connect (btn, "clicked", G_CALLBACK (zb_on_get_clicked), ctx);
    return btn;
}

/* (j) Append a named-preset combo for a numeric expose into @cell: a leading
 * "Preset…" placeholder followed by one row per { name, value } in @presets,
 * each row's id the stringified value.  Picking a row drives @spin to that
 * value; the combo itself is write-only (not a tracked control) and snaps
 * back to the placeholder via no seed, so it never fights the live value. */
static void
zb_on_preset_changed (GtkComboBox *combo, gpointer user_data)
{
    GtkSpinButton *spin = user_data;
    const gchar   *id   = gtk_combo_box_get_active_id (combo);

    if (id == NULL || *id == '\0')
        return;
    gtk_spin_button_set_value (spin, g_ascii_strtod (id, NULL));
    /* Return to the placeholder so the same preset can be re-picked and the
     * combo never misrepresents the spin's (independently edited) value. */
    gtk_combo_box_set_active (combo, 0);
}

static void
zb_add_numeric_presets (GtkWidget *cell, GtkWidget *spin, JsonArray *presets)
{
    GtkWidget *combo = pn_device_combo_new ();
    guint      n     = json_array_get_length (presets), i;

    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), "", "Preset…");
    for (i = 0; i < n; i++)
    {
        JsonNode    *elem = json_array_get_element (presets, i);
        JsonObject  *po;
        const gchar *name;
        gchar        id[G_ASCII_DTOSTR_BUF_SIZE];

        if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
            continue;
        po   = json_node_get_object (elem);
        name = zb_peek_string (po, "name");
        g_ascii_dtostr (id, sizeof id, zb_peek_double (po, "value", 0.0));
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), id,
                                   name != NULL ? name : id);
    }
    gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
    gtk_widget_set_tooltip_text (combo, "Jump to a named preset value");
    g_signal_connect (combo, "changed",
                      G_CALLBACK (zb_on_preset_changed), spin);
    gtk_box_pack_start (GTK_BOX (cell), combo, FALSE, FALSE, 0);
}

/* (e) Map one leaf expose to a row at *@row of @grid, honouring the
 * access bitmap: a settable expose (bit 2) gets its typed editable
 * control, anything else a read-only value row.  An unknown type also
 * falls back to a read-only raw-value row so a newer Z2M expose surfaces
 * rather than vanishing.  The control is tracked under its resolved key
 * for seeding.  A settable control is also filed under @page_controls
 * (borrowed; may be NULL) so the page's Apply can publish it. */
static void
zb_build_expose_row (ZbDevCtx *ctx, GtkGrid *grid, gint *row,
                     JsonObject *exp, GPtrArray *page_controls,
                     ZbTarget target, gboolean force_settable)
{
    const gchar *type     = zb_peek_string (exp, "type");
    const gchar *label    = zb_expose_label (exp);
    const gchar *desc     = zb_peek_string (exp, "description");
    gint         access   = zb_expose_access (exp);
    gboolean     settable = (access & 2) != 0 || force_settable;
    gboolean     gettable = (access & 4) != 0;
    gchar       *key      = zb_expose_key (exp);
    gint         used     = *row;   /* the row these cells land in (j: get) */
    GtkWidget   *cell;
    GtkWidget   *w        = NULL;
    ZbCtlKind    kind     = ZB_CTL_READONLY;
    gchar       *value_on = NULL, *value_off = NULL;

    if (settable && g_strcmp0 (type, "binary") == 0)
    {
        cell = pn_device_form_attach_control_row (grid, (*row)++, label);
        w = gtk_switch_new ();
        gtk_widget_set_halign (w, GTK_ALIGN_START);
        gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
        kind      = ZB_CTL_BINARY;
        value_on  = g_strdup (zb_peek_string (exp, "value_on"));
        value_off = g_strdup (zb_peek_string (exp, "value_off"));
    }
    else if (settable && g_strcmp0 (type, "numeric") == 0)
    {
        gdouble      min, max, step;
        const gchar *unit    = zb_peek_string (exp, "unit");
        JsonArray   *presets = zb_object_array (exp, "presets");
        gint         digits  = 0;
        gdouble      s;

        zb_numeric_range (exp, &min, &max, &step);
        cell = pn_device_form_attach_control_row (grid, (*row)++, label);
        w = pn_device_spin_new_with_range (min, max, step);
        for (s = step; s < 1.0 && digits < 6; s *= 10.0)
            digits++;
        gtk_spin_button_set_digits (GTK_SPIN_BUTTON (w), digits);
        gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
        if (unit != NULL && *unit != '\0')
        {
            gchar           *u  = g_strconcat (" ", unit, NULL);
            GtkWidget       *sl = gtk_label_new (u);
            GtkStyleContext *sc = gtk_widget_get_style_context (sl);
            gtk_style_context_add_class (sc, "dim-label");
            gtk_box_pack_start (GTK_BOX (cell), sl, FALSE, FALSE, 0);
            g_free (u);
        }
        if (presets != NULL && json_array_get_length (presets) > 0)
            zb_add_numeric_presets (cell, w, presets);   /* (j) */
        kind = ZB_CTL_NUMERIC;
    }
    else if (settable && g_strcmp0 (type, "enum") == 0)
    {
        JsonArray *values = zb_object_array (exp, "values");

        cell = pn_device_form_attach_control_row (grid, (*row)++, label);
        w = pn_device_combo_new ();
        if (values != NULL)
        {
            guint n = json_array_get_length (values), i;
            for (i = 0; i < n; i++)
            {
                gchar *v = zb_json_to_display (json_array_get_element (values, i));
                gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (w), v, v);
                g_free (v);
            }
        }
        gtk_box_pack_start (GTK_BOX (cell), w, TRUE, TRUE, 0);
        kind = ZB_CTL_ENUM;
    }
    else if (settable && g_strcmp0 (type, "text") == 0)
    {
        w = GTK_WIDGET (pn_device_form_attach_entry_row (grid, (*row)++, label));
        kind = ZB_CTL_TEXT;
    }
    else
    {
        /* Read-only value row: a non-settable expose, or a settable one
         * of a type we have no editor for (surfaced read-only rather than
         * dropped). */
        w = GTK_WIDGET (pn_device_form_attach_label_row (grid, (*row)++, label));
        kind = ZB_CTL_READONLY;
    }

    if (desc != NULL && w != NULL)
        gtk_widget_set_tooltip_text (w, desc);

    /* (j) A gettable expose (access bit 4) gets a refresh button in its own
     * column 2, so the per-control "re-read from the device" affordances line
     * up on the right edge whatever the row's control type.  The hexpanding
     * value cell at column 1 pushes them flush right.  Only live-state (SET)
     * controls support "<name>/get"; device options have no get endpoint. */
    if (key != NULL && gettable && target == ZB_TGT_SET)
        gtk_grid_attach (grid, zb_make_get_button (ctx, key), 2, used, 1, 1);

    if (key != NULL)
    {
        ZbControl *c = zb_track_control (ctx, key, kind, target, w,
                                         value_on, value_off);

        /* Device-option controls have no live source to seed from (item f
         * only paints from the per-device state topic, which carries the
         * set-properties); seed them from the expose's value_default at build
         * time so the snapshot baseline is the device's default and only
         * genuine edits publish. */
        if (kind != ZB_CTL_READONLY && target == ZB_TGT_OPTIONS &&
            json_object_has_member (exp, "value_default"))
            zb_seed_control_node (c, json_object_get_member (exp,
                                                             "value_default"));

        if (page_controls != NULL && kind != ZB_CTL_READONLY)
            g_ptr_array_add (page_controls, c);
    }
    else
    {
        g_free (key);
        g_free (value_on);
        g_free (value_off);
    }
}

/* Add @grid to @inner as an expander section titled @title, with a short dim
 * @desc line below the title (inside the section body, above the grid) -- the
 * "few words under each foldable" the dialog uses to explain every section.
 * @desc may be NULL.  Sinks @grid (via pn_device_form_add_section). */
static void
zb_add_section_desc (GtkWidget *inner, const gchar *title, const gchar *desc,
                     GtkWidget *grid)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

    if (desc != NULL)
    {
        GtkWidget       *d  = gtk_label_new (desc);
        GtkStyleContext *sc = gtk_widget_get_style_context (d);

        gtk_label_set_xalign     (GTK_LABEL (d), 0.0);
        gtk_label_set_line_wrap  (GTK_LABEL (d), TRUE);
        gtk_style_context_add_class (sc, "dim-label");
        gtk_box_pack_start (GTK_BOX (box), d, FALSE, FALSE, 0);
    }
    gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);
    pn_device_form_add_section (inner, title, box);   /* sinks box (+ grid) */
}

/* Append a read-only "key | value" row to @grid for @value (owned, freed
 * here), or nothing when @value is NULL -- the identity / options rows. */
static void
zb_add_readonly_row (GtkGrid *grid, gint *row, const gchar *key, gchar *value)
{
    GtkLabel *label;

    if (value == NULL)
        return;
    label = pn_device_form_attach_label_row (grid, (*row)++, key);
    pn_device_form_set_value (label, value);
    g_free (value);
}

/* ------------------------------------------------------------------ */
/*  Device page: editable device-level (all-devices) settings          */
/* ------------------------------------------------------------------ */

/* Build a row with @label and a PnInlineEditLabel on the right, seeded from
 * @seed (borrowed, may be NULL).  Returns the inline-edit widget so the caller
 * wires its ::committed handler.  Unlike a tracked control, an inline edit
 * commits live on Enter / focus-out, so it is not part of a page's Apply set. */
static GtkWidget *
zb_add_inline_row (GtkGrid *grid, gint *row, const gchar *label,
                   const gchar *seed, const gchar *tip)
{
    GtkWidget *cell = pn_device_form_attach_control_row (grid, (*row)++, label);
    GtkWidget *edit = pn_inline_edit_label_new ();

    if (seed != NULL)
        pn_inline_edit_label_set_text (PN_INLINE_EDIT_LABEL (edit), seed);
    if (tip != NULL)
        gtk_widget_set_tooltip_text (edit, tip);
    gtk_box_pack_start (GTK_BOX (cell), edit, TRUE, TRUE, 0);
    return edit;
}

/* The friendly name was edited inline.  Rename the device (the new name has no
 * other Z2M channel) unless it is empty or unchanged -- ::committed fires on
 * every Enter / focus-out, so skip the no-op.  selected_name holds the current
 * name and is retargeted optimistically; Z2M re-publishes bridge/devices on
 * success, refreshing the list. */
static void
zb_on_name_committed (PnInlineEditLabel *edit, const gchar *text,
                      gpointer user_data)
{
    ZbDevCtx   *ctx = user_data;
    JsonObject *req;

    (void) edit;
    if (ctx->selected_name == NULL || text == NULL || *text == '\0')
        return;
    if (g_strcmp0 (text, ctx->selected_name) == 0)
        return;
    if (ctx->sink == NULL)
    {
        pn_device_dialog_set_status (
                ctx->shell, "Pick a broker and press Apply before renaming.");
        return;
    }

    req = json_object_new ();
    json_object_set_string_member (req, "from", ctx->selected_name);
    json_object_set_string_member (req, "to",   text);
    zb_publish_to (ctx, ZB_DEV_RENAME_TOPIC, req);    /* consumes req */

    pn_device_dialog_set_statusf (ctx->shell, "Renaming %s to %s…",
                                  ctx->selected_name, text);
    g_free (ctx->selected_name);
    ctx->selected_name = g_strdup (text);
}

/* The description was edited inline.  Write it through device/options, skipping
 * a no-op against the last-committed value carried on the widget. */
static void
zb_on_desc_committed (PnInlineEditLabel *edit, const gchar *text,
                      gpointer user_data)
{
    ZbDevCtx    *ctx  = user_data;
    const gchar *base = g_object_get_data (G_OBJECT (edit),
                                           ZB_DESC_BASELINE_QDATA);
    JsonObject  *req, *opts;

    if (ctx->selected_name == NULL)
        return;
    if (text == NULL)
        text = "";
    if (g_strcmp0 (text, base != NULL ? base : "") == 0)
        return;
    if (ctx->sink == NULL)
    {
        pn_device_dialog_set_status (
                ctx->shell, "Pick a broker and press Apply before editing.");
        return;
    }

    opts = json_object_new ();
    json_object_set_string_member (opts, "description", text);
    req  = json_object_new ();
    json_object_set_string_member (req, "id", ctx->selected_name);
    json_object_set_object_member (req, "options", opts);   /* transfers opts */
    zb_publish_to (ctx, ZB_DEV_OPTIONS_TOPIC, req);         /* consumes req */

    g_object_set_data_full (G_OBJECT (edit), ZB_DESC_BASELINE_QDATA,
                            g_strdup (text), g_free);
    pn_device_dialog_set_statusf (ctx->shell, "Updated description of %s.",
                                  ctx->selected_name);
}

/* Build an editable boolean switch row bound to @key (device options channel),
 * seeded to @on, tracked and filed under @page.  value_on/off stay NULL so the
 * write emits a JSON bool -- the shape device/options expects. */
static void
zb_add_switch_control (ZbDevCtx *ctx, GtkGrid *grid, gint *row, GPtrArray *page,
                       const gchar *label, const gchar *key, gboolean on,
                       const gchar *tip)
{
    GtkWidget *cell = pn_device_form_attach_control_row (grid, (*row)++, label);
    GtkWidget *sw   = gtk_switch_new ();
    ZbControl *c;

    gtk_widget_set_halign (sw, GTK_ALIGN_START);
    gtk_switch_set_active (GTK_SWITCH (sw), on);
    gtk_box_pack_start (GTK_BOX (cell), sw, FALSE, FALSE, 0);
    if (tip != NULL)
        gtk_widget_set_tooltip_text (sw, tip);
    c = zb_track_control (ctx, g_strdup (key), ZB_CTL_BINARY, ZB_TGT_OPTIONS,
                          sw, NULL, NULL);
    g_ptr_array_add (page, c);
}

/* Build an editable integer spin row bound to @key (device options channel),
 * seeded to @value, with an optional dim @unit suffix, tracked under @page. */
static void
zb_add_int_control (ZbDevCtx *ctx, GtkGrid *grid, gint *row, GPtrArray *page,
                    const gchar *label, const gchar *key, gdouble value,
                    gdouble min, gdouble max, const gchar *unit,
                    const gchar *tip)
{
    GtkWidget *cell = pn_device_form_attach_control_row (grid, (*row)++, label);
    GtkWidget *spin = pn_device_spin_new_with_range (min, max, 1);
    ZbControl *c;

    gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin), 0);
    gtk_spin_button_set_value  (GTK_SPIN_BUTTON (spin), value);
    gtk_box_pack_start (GTK_BOX (cell), spin, FALSE, FALSE, 0);
    if (unit != NULL && *unit != '\0')
    {
        gchar           *u  = g_strconcat (" ", unit, NULL);
        GtkWidget       *sl = gtk_label_new (u);
        GtkStyleContext *sc = gtk_widget_get_style_context (sl);
        gtk_style_context_add_class (sc, "dim-label");
        gtk_box_pack_start (GTK_BOX (cell), sl, FALSE, FALSE, 0);
        g_free (u);
    }
    if (tip != NULL)
        gtk_widget_set_tooltip_text (spin, tip);
    c = zb_track_control (ctx, g_strdup (key), ZB_CTL_NUMERIC, ZB_TGT_OPTIONS,
                          spin, NULL, NULL);
    g_ptr_array_add (page, c);
}

/* Publish a device/remove request for the selected device. */
static void
zb_do_remove (ZbDevCtx *ctx, gboolean force)
{
    JsonObject *req;

    if (ctx->sink == NULL)
    {
        pn_device_dialog_set_status (
                ctx->shell, "Pick a broker and press Apply before removing.");
        return;
    }
    if (ctx->selected_name == NULL)
        return;

    req = json_object_new ();
    json_object_set_string_member  (req, "id",    ctx->selected_name);
    json_object_set_boolean_member (req, "force", force);
    zb_publish_to (ctx, ZB_DEV_REMOVE_TOPIC, req);    /* consumes req */

    pn_device_dialog_set_statusf (ctx->shell, "Requested removal of %s…",
                                  ctx->selected_name);
}

/* Confirm, then remove the selected device.  The modal warning carries a
 * "Force" check (remove from the Z2M database even if the device does not
 * respond on-air -- the escape hatch for an already-dead device). */
static void
zb_on_remove_clicked (GtkButton *button, gpointer user_data)
{
    ZbDevCtx  *ctx = user_data;
    GtkWidget *parent, *dialog, *area, *force_chk, *rm;
    gint       resp;

    (void) button;
    if (ctx->selected_name == NULL || ctx->shell == NULL)
        return;

    parent = pn_device_dialog_get_dialog (ctx->shell);
    dialog = gtk_message_dialog_new (
            GTK_WINDOW (parent),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
            "Remove \xe2\x80\x9c%s\xe2\x80\x9d from the Zigbee network?",
            ctx->selected_name);
    gtk_message_dialog_format_secondary_text (
            GTK_MESSAGE_DIALOG (dialog),
            "The device will be unpaired and removed from Zigbee2MQTT. "
            "This cannot be undone -- you would have to re-pair it to use "
            "it again.");

    force_chk = gtk_check_button_new_with_label (
            "Force: remove from the database even if the device does not "
            "respond");
    area = gtk_message_dialog_get_message_area (GTK_MESSAGE_DIALOG (dialog));
    gtk_box_pack_start (GTK_BOX (area), force_chk, FALSE, FALSE, 0);
    gtk_widget_show (force_chk);

    gtk_dialog_add_button (GTK_DIALOG (dialog), "Cancel", GTK_RESPONSE_CANCEL);
    rm = gtk_dialog_add_button (GTK_DIALOG (dialog), "Remove",
                                GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class (gtk_widget_get_style_context (rm),
                                 "destructive-action");
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_CANCEL);

    resp = gtk_dialog_run (GTK_DIALOG (dialog));
    if (resp == GTK_RESPONSE_ACCEPT)
        zb_do_remove (ctx, gtk_toggle_button_get_active (
                              GTK_TOGGLE_BUTTON (force_chk)));
    gtk_widget_destroy (dialog);
}

/* The always-present "Device" page (first device page, generic -> special):
 * a "Device" section with the friendly name + description edited inline and
 * committed live (rename / device options), a read-only "Info" section off the
 * inventory record, and a separate "Remove device" foldable.  The generic
 * per-device options live on the Options page, not here. */
static void
zb_build_device_page (ZbDevCtx *ctx, JsonObject *dev)
{
    GtkWidget   *inner;
    GtkWidget   *tab   = pn_device_form_new_tab (&inner);
    GtkWidget   *grid  = gtk_grid_new ();
    GtkWidget   *info  = gtk_grid_new ();
    GtkWidget   *rmbox, *remove, *name_edit, *desc_edit;
    JsonObject  *def   = NULL;
    JsonNode    *dn;
    const gchar *fname = zb_peek_string (dev, "friendly_name");
    const gchar *descr = zb_peek_string (dev, "description");
    gint         row   = 0;

    /* Device: identity, edited inline and committed live. */
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    name_edit = zb_add_inline_row (GTK_GRID (grid), &row, "Friendly name",
                                   fname,
                                   "The name this device is published under "
                                   "(zigbee2mqtt/<name>).");
    g_signal_connect (name_edit, "committed",
                      G_CALLBACK (zb_on_name_committed), ctx);
    desc_edit = zb_add_inline_row (GTK_GRID (grid), &row, "Description", descr,
                                   "Free-text note stored with the device.");
    g_object_set_data_full (G_OBJECT (desc_edit), ZB_DESC_BASELINE_QDATA,
                            g_strdup (descr != NULL ? descr : ""), g_free);
    g_signal_connect (desc_edit, "committed",
                      G_CALLBACK (zb_on_desc_committed), ctx);
    zb_add_section_desc (inner, "Device",
                         "Give this device a friendly name and an optional "
                         "note so you can recognise it later. The name labels "
                         "it everywhere in the system, so something like "
                         "\"Living room lamp\" works well. Click the pencil, "
                         "type, and press Enter to save \xe2\x80\x94 each field "
                         "saves on its own.", grid);

    /* Info: read-only addressing / vendor. */
    gtk_grid_set_row_spacing    (GTK_GRID (info), 8);
    gtk_grid_set_column_spacing (GTK_GRID (info), 12);
    row = 0;
    zb_add_readonly_row (GTK_GRID (info), &row, "IEEE address",
                         zb_peek_display (dev, "ieee_address"));
    zb_add_readonly_row (GTK_GRID (info), &row, "Type",
                         zb_peek_display (dev, "type"));
    zb_add_readonly_row (GTK_GRID (info), &row, "Network address",
                         zb_peek_display (dev, "network_address"));
    zb_add_readonly_row (GTK_GRID (info), &row, "Power source",
                         zb_peek_display (dev, "power_source"));
    if (json_object_has_member (dev, "definition"))
    {
        dn = json_object_get_member (dev, "definition");
        if (dn != NULL && JSON_NODE_HOLDS_OBJECT (dn))
            def = json_node_get_object (dn);
    }
    if (def != NULL)
    {
        zb_add_readonly_row (GTK_GRID (info), &row, "Vendor",
                             zb_peek_display (def, "vendor"));
        zb_add_readonly_row (GTK_GRID (info), &row, "Model",
                             zb_peek_display (def, "model"));
    }
    if (row > 0)
    {
        zb_add_section_desc (inner, "Info",
                             "These are the device's fixed technical details: "
                             "its unique hardware address (a bit like a serial "
                             "number), the kind of device, and who made it. "
                             "Nothing here can be changed \xe2\x80\x94 it is "
                             "shown only so you can tell exactly which device "
                             "this is, and it helps if you ever ask for "
                             "support.", info);
    }
    else
    {
        g_object_ref_sink (info);
        gtk_widget_destroy (info);
        g_object_unref (info);
    }

    /* Remove: its own foldable section. */
    rmbox  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign (rmbox, GTK_ALIGN_START);
    remove = gtk_button_new_with_label ("Remove device\xe2\x80\xa6");
    gtk_style_context_add_class (gtk_widget_get_style_context (remove),
                                 "destructive-action");
    gtk_widget_set_tooltip_text (remove,
            "Unpair this device and remove it from Zigbee2MQTT.");
    g_signal_connect (remove, "clicked",
                      G_CALLBACK (zb_on_remove_clicked), ctx);
    gtk_box_pack_start (GTK_BOX (rmbox), remove, FALSE, FALSE, 0);
    zb_add_section_desc (inner, "Remove device",
                         "Removing a device disconnects it from your wireless "
                         "network and erases it from the system. Do this if you "
                         "no longer use it, or want to set it up elsewhere. "
                         "This cannot be undone: to use it again you would have "
                         "to add it back from scratch (\"pairing\"), usually by "
                         "pressing a button on the device. If it is broken or "
                         "already gone, the confirmation box offers a \"force\" "
                         "option that removes it anyway.", rmbox);

    pn_device_dialog_append_page (ctx->shell, tab, "Device");
    g_ptr_array_add (ctx->device_pages, tab);
    gtk_widget_show_all (tab);
}

/* Fill @grid with the generic per-device options every device shares (the
 * "all-devices" properties), as editable controls routed to device/options.
 * `disabled` seeds accurately from the inventory record; the rest have no live
 * source over MQTT and seed from their Z2M defaults -- only a value the user
 * changes is sent (device/options merges), so a default shown never overwrites
 * an unseen real value.  Controls are filed under @page for the page Apply. */
static void
zb_add_generic_options (ZbDevCtx *ctx, GtkGrid *grid, JsonObject *dev,
                        GPtrArray *page)
{
    JsonNode *dis = json_object_has_member (dev, "disabled")
                        ? json_object_get_member (dev, "disabled") : NULL;
    gint      row = 0;

    zb_add_switch_control (ctx, grid, &row, page, "Disabled", "disabled",
                           zb_binary_is_on (dis, NULL),
                           "Exclude the device from network scans, "
                           "availability and group state.");
    zb_add_switch_control (ctx, grid, &row, page, "Retain", "retain", FALSE,
                           "Retain this device's MQTT messages.");
    zb_add_int_control (ctx, grid, &row, page, "Retention", "retention",
                        0, 0, 1000000, "s",
                        "MQTT message expiry (requires retain + MQTT v5).");
    zb_add_switch_control (ctx, grid, &row, page, "Optimistic", "optimistic",
                           TRUE, "Publish optimistic state after a /set.");
    zb_add_int_control (ctx, grid, &row, page, "Debounce", "debounce",
                        0, 0, 1000000, "s", "Debounce this device's messages.");
    zb_add_int_control (ctx, grid, &row, page, "Throttle", "throttle",
                        0, 0, 1000000, "s",
                        "Minimum time between published payloads.");
    zb_add_int_control (ctx, grid, &row, page, "QoS", "qos", 0, 0, 2, NULL,
                        "MQTT QoS level for this device's messages.");
}

/* The always-present, editable "Options" page (last device page, most
 * special).  Leads with the generic per-device options (above), then -- when
 * the device's definition carries options (transition / legacy / invert_cover
 * …) -- a second section built from them, also editable and routed to
 * device/options, seeded from value_default.  One page Apply publishes both;
 * force_settable=TRUE since option exposes often omit the access bitmap. */
static void
zb_build_options_page (ZbDevCtx *ctx, JsonObject *dev, JsonArray *options)
{
    GtkWidget *inner;
    GtkWidget *tab  = pn_device_form_new_tab (&inner);
    GtkWidget *gen  = gtk_grid_new ();
    GPtrArray *page = g_ptr_array_new ();      /* borrowed editable controls */
    guint      n    = (options != NULL) ? json_array_get_length (options) : 0;
    guint      i;

    gtk_grid_set_row_spacing    (GTK_GRID (gen), 8);
    gtk_grid_set_column_spacing (GTK_GRID (gen), 12);
    zb_add_generic_options (ctx, GTK_GRID (gen), dev, page);
    zb_add_section_desc (inner, "Options (defaults shown)",
                         "These are advanced, behind-the-scenes settings for "
                         "how the system handles this device's messages \xe2\x80\x94 "
                         "such as how long to remember its last status. Most "
                         "people never need them, so the safe choice is to "
                         "leave them alone. Because the system cannot read "
                         "these values back, the boxes show the usual defaults "
                         "rather than what is in effect; only a box you change "
                         "is sent, so anything you leave alone stays as it "
                         "was.", gen);

    if (n > 0)
    {
        GtkWidget *grid = gtk_grid_new ();
        gint       row  = 0;

        gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
        gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
        for (i = 0; i < n; i++)
        {
            JsonNode *elem = json_array_get_element (options, i);

            if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
                continue;
            zb_build_expose_row (ctx, GTK_GRID (grid), &row,
                                 json_node_get_object (elem), page,
                                 ZB_TGT_OPTIONS, TRUE);
        }
        if (row > 0)
        {
            zb_add_section_desc (inner, "Device options (defaults shown)",
                                 "These extra options come from the device "
                                 "itself and fine-tune how it behaves \xe2\x80\x94 "
                                 "such as how smoothly a light fades. As with "
                                 "the section above, the boxes show the usual "
                                 "defaults rather than the current values, and "
                                 "only a box you change is sent. If you are "
                                 "unsure what one does, leave it alone.", grid);
        }
        else
        {
            g_object_ref_sink (grid);
            gtk_widget_destroy (grid);
            g_object_unref (grid);
        }
    }

    /* The generic section always adds controls, so the Apply is always wired. */
    zb_add_page_apply (ctx, inner, page);      /* (g) consumes page */
    pn_device_dialog_append_page (ctx->shell, tab, "Options");
    g_ptr_array_add (ctx->device_pages, tab);
    gtk_widget_show_all (tab);
}

/* Build one "Settings"-style page titled @title from the bare exposes in
 * @exps, with its own borrowed control list + Apply button. */
static void
zb_build_settings_page (ZbDevCtx *ctx, GPtrArray *exps, const gchar *title)
{
    GtkWidget *inner;
    GtkWidget *tab  = pn_device_form_new_tab (&inner);
    GtkWidget *grid = gtk_grid_new ();
    GPtrArray *page = g_ptr_array_new ();   /* borrowed settable controls */
    gint       row  = 0;
    guint      i;

    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    for (i = 0; i < exps->len; i++)
        zb_build_expose_row (ctx, GTK_GRID (grid), &row,
                             g_ptr_array_index (exps, i), page,
                             ZB_TGT_SET, FALSE);

    zb_add_section_desc (inner, title,
                         "These are the adjustable features this device offers "
                         "\xe2\x80\x94 the list depends on what it can do, so it "
                         "differs from one device to the next. Changing a value "
                         "sends it straight to the device, which should react "
                         "within a second or two. If a value springs back, the "
                         "device refused or adjusted it.", grid);
    zb_add_page_apply (ctx, inner, page);   /* (g) consumes page */
    pn_device_dialog_append_page (ctx->shell, tab, title);
    g_ptr_array_add (ctx->device_pages, tab);
    gtk_widget_show_all (tab);
}

/* (j) Turn the bare (featureless) top-level exposes into Settings pages,
 * grouped by their `endpoint`: a multi-gang device whose gangs surface as
 * flat per-endpoint exposes (rather than composite features) then lands one
 * page per endpoint -- "Settings (left)" / "Settings (right)" -- instead of
 * one undifferentiated list.  The endpoint-less exposes share the "" group.
 * First-seen order is preserved.  The common single-group case (one endpoint,
 * or none) keeps the plain "Settings" title, identical to the pre-grouping
 * behaviour. */
static void
zb_build_bare_pages (ZbDevCtx *ctx, GPtrArray *bare)
{
    GPtrArray  *order  = g_ptr_array_new ();   /* endpoint keys, first-seen */
    GHashTable *groups = g_hash_table_new (g_str_hash, g_str_equal);
    guint       i;

    if (bare->len == 0)
    {
        g_hash_table_destroy (groups);
        g_ptr_array_unref (order);
        return;
    }

    for (i = 0; i < bare->len; i++)
    {
        JsonObject  *exp = g_ptr_array_index (bare, i);
        const gchar *ep  = zb_peek_string (exp, "endpoint");
        const gchar *key = (ep != NULL && *ep != '\0') ? ep : "";
        GPtrArray   *grp = g_hash_table_lookup (groups, key);

        if (grp == NULL)
        {
            grp = g_ptr_array_new ();
            g_hash_table_insert (groups, (gpointer) key, grp);
            g_ptr_array_add (order, (gpointer) key);   /* borrowed JSON string */
        }
        g_ptr_array_add (grp, exp);
    }

    for (i = 0; i < order->len; i++)
    {
        const gchar *key   = g_ptr_array_index (order, i);
        GPtrArray   *grp   = g_hash_table_lookup (groups, key);
        gchar       *title = (*key != '\0' && order->len > 1)
                                 ? g_strdup_printf ("Settings (%s)", key)
                                 : g_strdup ("Settings");

        zb_build_settings_page (ctx, grp, title);
        g_free (title);
        g_ptr_array_unref (grp);
    }

    g_hash_table_destroy (groups);
    g_ptr_array_unref (order);
}

/* Walk the selected device into pages, ordered generic -> special after the
 * static "Broker" page: the editable "Device" page (identity + info + remove),
 * then the "Settings" page(s) built from the device's bare settable exposes,
 * then the editable "Options" page (device-level options + definition.options).
 * Top-level composite/typed exposes (switch/light/…) are intentionally skipped
 * -- this dialog configures the device, it does not operate it.  Then jump to
 * the Device page. */
static void
zb_build_device_pages (ZbDevCtx *ctx, JsonObject *dev,
                       JsonArray *exposes, JsonArray *options)
{
    GPtrArray *bare = g_ptr_array_new ();
    guint      n, i;

    /* Device page first. */
    zb_build_device_page (ctx, dev);

    /* Settings page(s) from the bare (leaf, settable) top-level exposes,
     * split by endpoint for a multi-gang device that surfaces them flat.  A
     * composite expose (one carrying `features`) is the operational control
     * (the on/off "switch" page) and is not shown here. */
    if (exposes != NULL)
    {
        n = json_array_get_length (exposes);
        for (i = 0; i < n; i++)
        {
            JsonNode   *elem = json_array_get_element (exposes, i);
            JsonObject *exp;

            if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
                continue;
            exp = json_node_get_object (elem);
            if (zb_object_array (exp, "features") == NULL)
                g_ptr_array_add (bare, exp);
        }
    }
    zb_build_bare_pages (ctx, bare);
    g_ptr_array_unref (bare);

    /* Options page last (device-level options + definition.options). */
    zb_build_options_page (ctx, dev, options);

    /* The static "Broker" page is index 0; show the Device page. */
    if (ctx->device_pages->len > 0)
        pn_device_dialog_set_current_page (ctx->shell, 1);
}

/* (f, h) Paint every built control from the captured device-state object
 * by its resolved key, leaving the build-time default (or the "—"
 * placeholder on read-only rows) where the value is absent.  Each editable
 * control it paints also adopts the painted value as its baseline, so
 * change-detection stays anchored to the device's truth.  Used both for
 * the initial seed on select (item f) and to repaint from a confirmed
 * re-publish (item h, via zb_capture_state) -- a value the device clamped
 * or rejected then shows through, replacing what was typed.  Controls
 * absent from @state (and any pending edit on them) are left untouched. */
static void
zb_seed_controls (ZbDevCtx *ctx)
{
    JsonNode   *snode;
    JsonObject *state;
    guint       i;

    if (ctx->selected_name == NULL)
        return;
    snode = g_hash_table_lookup (ctx->state, ctx->selected_name);
    if (snode == NULL || !JSON_NODE_HOLDS_OBJECT (snode))
        return;
    state = json_node_get_object (snode);

    for (i = 0; i < ctx->controls->len; i++)
    {
        ZbControl *c = g_ptr_array_index (ctx->controls, i);
        JsonNode  *v;

        if (!json_object_has_member (state, c->key))
            continue;
        v = json_object_get_member (state, c->key);
        if (v == NULL)
            continue;

        zb_seed_control_node (c, v);

        /* The painted value is now the device's confirmed truth: anchor
         * the baseline to it so a later Apply diffs against what the device
         * actually holds, not the pre-paint value. */
        if (c->kind != ZB_CTL_READONLY)
        {
            g_free (c->baseline);
            c->baseline = zb_control_read_string (c);
        }
    }
}

/* (g) Snapshot every editable control's just-seeded value as its baseline,
 * so the first Apply publishes only what the user actually changes.  Run
 * after zb_seed_controls -- including for controls left at their build
 * default because no live value was captured. */
static void
zb_snapshot_baselines (ZbDevCtx *ctx)
{
    guint i;

    for (i = 0; i < ctx->controls->len; i++)
    {
        ZbControl *c = g_ptr_array_index (ctx->controls, i);

        if (c->kind == ZB_CTL_READONLY)
            continue;
        g_free (c->baseline);
        c->baseline = zb_control_read_string (c);
    }
}

/* A device row was clicked.  Tear down the previously-shown device pages,
 * resolve the clicked row back to its inventory record and stash its
 * friendly name + exposes/options for the page builder.  @row is borrowed;
 * its fields come straight from the list we built. */
static void
zb_on_device_selected (const PnDeviceRow *row, gpointer user_data)
{
    ZbDevCtx    *ctx = user_data;
    JsonObject  *dev;
    JsonArray   *exposes, *options;
    const gchar *fname;
    guint        n_exposes;

    if (row == NULL)
        return;

    /* (c) Drop the old device's pages before building the new one's. */
    zb_clear_device_pages (ctx);
    g_clear_pointer (&ctx->selected_name, g_free);

    /* (b) Resolve the row id (IEEE) back to its device record. */
    dev = zb_find_device (ctx, row->id);
    if (dev == NULL)
    {
        pn_device_dialog_set_statusf (
                ctx->shell, "%s  (%s) — no matching device record.",
                row->primary != NULL ? row->primary : "device",
                row->id != NULL ? row->id : "?");
        return;
    }

    fname = zb_peek_string (dev, "friendly_name");
    ctx->selected_name = g_strdup (fname != NULL ? fname : row->primary);

    exposes = zb_definition_array (dev, "exposes");
    options = zb_definition_array (dev, "options");

    /* (d, e) walk the exposes tree into editable settings pages (plus the
     * read-only identity / options pages of item i), then (f) seed every
     * built control from the captured live state and (g) snapshot baselines
     * so each page's Apply publishes only changes. */
    zb_build_device_pages (ctx, dev, exposes, options);
    zb_seed_controls (ctx);
    zb_snapshot_baselines (ctx);

    n_exposes = (exposes != NULL) ? json_array_get_length (exposes) : 0;
    pn_device_dialog_set_statusf (
            ctx->shell, "%s — %u expose%s%s.",
            ctx->selected_name != NULL ? ctx->selected_name : "device",
            n_exposes, n_exposes == 1 ? "" : "s",
            options != NULL ? ", options available" : "");
}

/* ------------------------------------------------------------------ */
/*  Pairing (join window)                                              */
/* ------------------------------------------------------------------ */

/* The number of devices in the current inventory (valid records only), or 0
 * when none is held -- the baseline the join dialog counts arrivals against. */
static guint
zb_device_count (ZbDevCtx *ctx)
{
    JsonArray *arr;
    guint      n, i, count = 0;

    if (ctx->devices == NULL || !JSON_NODE_HOLDS_ARRAY (ctx->devices))
        return 0;
    arr = json_node_get_array (ctx->devices);
    n   = json_array_get_length (arr);
    for (i = 0; i < n; i++)
    {
        JsonNode *e = json_array_get_element (arr, i);
        if (e != NULL && JSON_NODE_HOLDS_OBJECT (e))
            count++;
    }
    return count;
}

/* Publish a permit_join request: open the coordinator's join window for
 * @seconds when @open, or close it immediately otherwise.  A no-op without a
 * connected sink (the caller has already reported that). */
static void
zb_join_publish (ZbDevCtx *ctx, gboolean open, guint seconds)
{
    JsonObject *obj;

    if (ctx->sink == NULL)
        return;
    obj = json_object_new ();
    json_object_set_boolean_member (obj, "value", open);
    if (open)
        json_object_set_int_member (obj, "time", (gint64) seconds);
    zb_publish_to (ctx, ZB_DEV_PERMIT_TOPIC, obj);      /* consumes obj */
}

/* Repaint the open join dialog's progress bar and countdown / joined-count
 * label from the current ctx->join_* state.  A no-op when none is open. */
static void
zb_join_update (ZbDevCtx *ctx)
{
    gchar  *text;
    gdouble frac;

    if (ctx->join_dialog == NULL)
        return;
    frac = (ctx->join_total > 0)
               ? (gdouble) ctx->join_left / (gdouble) ctx->join_total
               : 0.0;
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (ctx->join_progress), frac);

    text = g_strdup_printf ("%u:%02u remaining \xe2\x80\x94 %u device%s joined.",
                            ctx->join_left / 60, ctx->join_left % 60,
                            ctx->join_joined,
                            ctx->join_joined == 1 ? "" : "s");
    gtk_label_set_text (GTK_LABEL (ctx->join_label), text);
    g_free (text);
}

/* A fresh inventory arrived: while a window is open, recompute how many
 * devices joined since it opened and repaint the dialog. */
static void
zb_join_on_inventory (ZbDevCtx *ctx)
{
    guint now;

    if (ctx->join_dialog == NULL)
        return;
    now = zb_device_count (ctx);
    ctx->join_joined = (now > ctx->join_base_count)
                           ? now - ctx->join_base_count : 0;
    zb_join_update (ctx);
}

/* Tear down the open join window.  Idempotent and the single teardown path:
 * the dialog's "response" and "destroy" handlers both route here and either
 * may fire first.  @user_ended is TRUE when the user cancelled / closed the
 * dialog (so we close the coordinator's window now) and FALSE on a natural
 * timer expiry (Z2M closes it itself) or app teardown.
 *
 * Crucially this does NOT run a nested main loop -- it just stops the timer,
 * stops the marker spinner and destroys the dialog, all on the main thread,
 * so the MQTT network thread's cross-thread g_idle_add can never contend with
 * a re-entrant teardown (the gtk_dialog_run deadlock this replaces). */
static void
zb_join_finish (ZbDevCtx *ctx, gboolean user_ended)
{
    GtkWidget *dialog = ctx->join_dialog;

    if (dialog == NULL)
        return;

    /* Forget the dialog first, so the destroy handler the gtk_widget_destroy
     * below triggers sees no open window and returns immediately -- this is
     * the one place that tears down. */
    ctx->join_dialog = NULL;

    if (ctx->join_tick_id != 0)
    {
        g_source_remove (ctx->join_tick_id);
        ctx->join_tick_id = 0;
    }

    /* Always close the coordinator's window.  Because we grant in <=254 s
     * chunks and re-arm, the most recent grant typically extends past the
     * window we promised, so even a natural timer expiry leaves the coordinator
     * accepting devices -- we must explicitly disable it.  A close when none is
     * open is a harmless no-op.  (@user_ended is kept for the caller's intent
     * but no longer gates this.) */
    (void) user_ended;
    zb_join_publish (ctx, FALSE, 0);

    if (ctx->join_spinner != NULL)
    {
        gtk_spinner_stop (GTK_SPINNER (ctx->join_spinner));
        gtk_widget_hide (ctx->join_spinner);
    }
    /* Skip the status update when the parent is itself being torn down (the
     * DESTROY_WITH_PARENT path): its status bar may already be gone. */
    if (ctx->shell != NULL)
    {
        GtkWidget *md = pn_device_dialog_get_dialog (ctx->shell);
        if (md != NULL && !gtk_widget_in_destruction (md))
            pn_device_dialog_set_statusf (
                    ctx->shell,
                    "Join window closed \xe2\x80\x94 %u device%s joined.",
                    ctx->join_joined, ctx->join_joined == 1 ? "" : "s");
    }

    ctx->join_progress = NULL;
    ctx->join_label    = NULL;
    gtk_widget_destroy (dialog);
}

/* Cancel button, Escape, or window-manager close: the user ended it.  The
 * timer's natural-expiry path responds with GTK_RESPONSE_NONE instead. */
static void
zb_join_on_response (GtkDialog *dialog, gint response, gpointer user_data)
{
    (void) dialog;
    zb_join_finish (user_data, response != GTK_RESPONSE_NONE);
}

/* Safety net for a destroy that did not come through "response" (e.g. the
 * parent torn down with DESTROY_WITH_PARENT): not a user cancel, so do not
 * publish a close. */
static void
zb_join_on_destroy (GtkWidget *dialog, gpointer user_data)
{
    (void) dialog;
    zb_join_finish (user_data, FALSE);
}

/* Per-second timer driving the countdown.  On expiry it asks the dialog to
 * respond with NONE (the natural-expiry marker), which routes into
 * zb_join_finish via the response handler. */
static gboolean
zb_join_tick (gpointer user_data)
{
    ZbDevCtx *ctx = user_data;

    if (ctx->join_left > 0)
        ctx->join_left--;
    if (ctx->join_grant_left > 0)
        ctx->join_grant_left--;

    /* Re-arm: a single permit_join lasts at most ZB_JOIN_CHUNK_SECONDS, so
     * before the current grant lapses -- and while enough window remains to be
     * worth it -- send a fresh grant to keep the coordinator accepting devices
     * across the whole ZB_JOIN_WINDOW_SECONDS the dialog promises. */
    if (ctx->join_left > ZB_JOIN_REARM_LEAD &&
        ctx->join_grant_left <= ZB_JOIN_REARM_LEAD)
    {
        zb_join_publish (ctx, TRUE, ZB_JOIN_CHUNK_SECONDS);
        ctx->join_grant_left = ZB_JOIN_CHUNK_SECONDS;
    }

    zb_join_update (ctx);

    if (ctx->join_left == 0)
    {
        ctx->join_tick_id = 0;          /* clear before finish double-removes */
        if (ctx->join_dialog != NULL)
            gtk_dialog_response (GTK_DIALOG (ctx->join_dialog),
                                 GTK_RESPONSE_NONE);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Open a 5-minute join window on the coordinator and show a *non-modal-loop*
 * countdown dialog (spinner + progress bar + remaining time + devices-joined
 * count + Cancel) while it is open.  Unlike gtk_dialog_run, this returns
 * immediately: the dialog drives itself through its "response"/"destroy"
 * handlers and the per-second timer, so the editor's main loop keeps pumping
 * MQTT traffic underneath it.  The window's modality (input grab) comes from
 * GTK_DIALOG_MODAL + transient-for-parent, not from desensitising the parent
 * (a missed re-enable would look like a freeze). */
static void
zb_on_join_clicked (GtkButton *button, gpointer user_data)
{
    ZbDevCtx  *ctx = user_data;
    GtkWidget *main_dialog, *dialog, *content, *box;
    GtkWidget *intro, *spin_row, *spinner, *progress, *label;

    (void) button;
    if (ctx->sink == NULL)
    {
        pn_device_dialog_set_status (
                ctx->shell, "Pick a broker and press Apply before pairing.");
        return;
    }
    if (ctx->join_dialog != NULL)       /* already open */
        return;

    main_dialog = pn_device_dialog_get_dialog (ctx->shell);
    dialog = gtk_dialog_new_with_buttons (
            "Pairing \xe2\x80\x94 join window open",
            GTK_WINDOW (main_dialog),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            "Cancel", GTK_RESPONSE_CANCEL,
            NULL);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 380, -1);

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width (GTK_CONTAINER (box), 16);
    gtk_box_pack_start (GTK_BOX (content), box, TRUE, TRUE, 0);

    intro = gtk_label_new (
            "The hub is accepting new devices. Put each device into pairing "
            "mode now \xe2\x80\x94 usually by holding its button or "
            "power-cycling it.");
    gtk_label_set_line_wrap (GTK_LABEL (intro), TRUE);
    gtk_label_set_xalign    (GTK_LABEL (intro), 0.0);
    gtk_box_pack_start (GTK_BOX (box), intro, FALSE, FALSE, 0);

    spinner = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (spinner));
    spin_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign (spin_row, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (spin_row), spinner, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), spin_row, FALSE, FALSE, 0);

    progress = gtk_progress_bar_new ();
    gtk_box_pack_start (GTK_BOX (box), progress, FALSE, FALSE, 0);

    label = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

    ctx->join_dialog     = dialog;
    ctx->join_progress   = progress;
    ctx->join_label      = label;
    ctx->join_total      = ZB_JOIN_WINDOW_SECONDS;
    ctx->join_left       = ZB_JOIN_WINDOW_SECONDS;
    ctx->join_grant_left = ZB_JOIN_CHUNK_SECONDS;   /* first grant below */
    ctx->join_base_count = zb_device_count (ctx);
    ctx->join_joined     = 0;
    zb_join_update (ctx);

    g_signal_connect (dialog, "response",
                      G_CALLBACK (zb_join_on_response), ctx);
    g_signal_connect (dialog, "destroy",
                      G_CALLBACK (zb_join_on_destroy), ctx);

    /* Open the window (first <=254 s grant; the tick re-arms it) and spin the
     * section marker; the dialog's own modality blocks the parent's input
     * without us desensitising it. */
    zb_join_publish (ctx, TRUE, ZB_JOIN_CHUNK_SECONDS);
    if (ctx->join_spinner != NULL)
    {
        gtk_widget_show (ctx->join_spinner);
        gtk_spinner_start (GTK_SPINNER (ctx->join_spinner));
    }
    pn_device_dialog_set_status (ctx->shell,
                                 "Join window open \xe2\x80\x94 pairing for "
                                 "5 minutes\xe2\x80\xa6");

    ctx->join_tick_id = g_timeout_add_seconds (1, zb_join_tick, ctx);

    gtk_widget_show_all (dialog);
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

    ctx->state = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, (GDestroyNotify) json_node_unref);
    ctx->device_pages = g_ptr_array_new ();
    ctx->controls     = g_ptr_array_new_with_free_func (zb_control_free);

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

    zb_add_section_desc (inner, "MQTT Broker",
                         "Your smart devices do not talk to this program "
                         "directly \xe2\x80\x94 their messages pass through a "
                         "small relay called an MQTT broker, rather like "
                         "letters through a post office. Choose which broker to "
                         "connect to and press Apply; \"Default\" uses the one "
                         "your system already set up, which is usually right. "
                         "Once connected, your devices appear in the list on "
                         "the left and the counter shows messages arriving.",
                         grid);

    /* Join-window section: opens a 5-minute pairing window on the
     * coordinator.  Its Start button stays insensitive until a broker is
     * applied (zb_start_source enables it; zb_drop_source disables it). */
    {
        GtkWidget *jgrid = gtk_grid_new ();
        GtkWidget *jcell;
        GtkWidget *start;
        GtkWidget *jspinner;
        gint       jrow = 0;

        gtk_grid_set_row_spacing    (GTK_GRID (jgrid), 8);
        gtk_grid_set_column_spacing (GTK_GRID (jgrid), 12);

        jcell = pn_device_form_attach_control_row (GTK_GRID (jgrid), jrow++,
                                                   "Pairing");
        start = gtk_button_new_with_label ("Open join window\xe2\x80\xa6");
        gtk_widget_set_tooltip_text (start,
                "Let new Zigbee devices join the network for 5 minutes.");
        gtk_widget_set_sensitive (start, FALSE);   /* until a broker applies */
        g_signal_connect (start, "clicked",
                          G_CALLBACK (zb_on_join_clicked), ctx);
        gtk_box_pack_start (GTK_BOX (jcell), start, FALSE, FALSE, 0);

        jspinner = gtk_spinner_new ();
        gtk_widget_set_no_show_all (jspinner, TRUE);   /* shown only when open */
        gtk_box_pack_start (GTK_BOX (jcell), jspinner, FALSE, FALSE, 0);

        ctx->join_start   = start;
        ctx->join_spinner = jspinner;

        zb_add_section_desc (inner, "Pairing (join window)",
                             "To add a new device, open a join window: for the "
                             "next five minutes the hub will accept devices "
                             "trying to connect. Press the button, then put each "
                             "new device into pairing mode (often by holding its "
                             "button). A progress window shows the countdown and "
                             "how many have joined, and lets you stop early. "
                             "This becomes available once a broker is connected "
                             "above.", jgrid);
    }

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

    /* Belt-and-suspenders: a running join window holds a nested main loop in
     * zb_on_join_clicked, so this normally cannot fire mid-window -- but never
     * leave a timer pointing at freed ctx. */
    if (ctx->join_tick_id != 0)
    {
        g_source_remove (ctx->join_tick_id);
        ctx->join_tick_id = 0;
    }

    zb_drop_source (ctx);
    g_clear_pointer (&ctx->devices, json_node_unref);

    /* Shell already cleared, so the pages are torn down by the dialog
     * itself -- just free our tracking containers and the state map. */
    g_clear_pointer (&ctx->controls, g_ptr_array_unref);
    g_clear_pointer (&ctx->device_pages, g_ptr_array_unref);
    g_clear_pointer (&ctx->state, g_hash_table_unref);
    g_clear_pointer (&ctx->selected_name, g_free);
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
