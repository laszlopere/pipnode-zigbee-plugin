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

/* The hidden source always watches the whole Zigbee2MQTT tree. */
#define ZB_DEV_SUBSCRIBE_TOPIC "zigbee2mqtt/#"

/* Common prefix of every Zigbee2MQTT topic.  A bare device-state topic is
 * ZB_DEV_TOPIC_PREFIX "<friendly_name>" with nothing further after it. */
#define ZB_DEV_TOPIC_PREFIX "zigbee2mqtt/"

/* Z2M publishes its full device inventory here, retained, as a JSON
 * array of device records -- delivered right after subscribe and again
 * whenever the device set changes.  We read it to populate the list. */
#define ZB_DEV_DEVICES_TOPIC "zigbee2mqtt/bridge/devices"

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

/* One built settings control, bound to the device-state JSON key it shows.
 * Tracked per-dialog and cleared in lockstep with the device pages (the
 * widget itself is owned by its notebook page; this record only borrows
 * it).  value_on/value_off carry a binary expose's string mapping so the
 * seed pass can turn "ON"/"OFF" (or a JSON bool) into a switch state. */
typedef struct
{
    gchar      *key;        /* resolved device-state property key (owned) */
    ZbCtlKind   kind;
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

    /* When this is the device on screen, item h repaints its live
     * controls from the refreshed state here (the initial seed at select
     * time is item f, in zb_seed_controls). */
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

/* Record a built control bound to @key for the seed / write-back passes.
 * Takes ownership of @key, @value_on and @value_off.  Returns the tracked
 * control (owned by ctx->controls) so the caller can also file it under a
 * page. */
static ZbControl *
zb_track_control (ZbDevCtx *ctx, gchar *key, ZbCtlKind kind,
                  GtkWidget *widget, gchar *value_on, gchar *value_off)
{
    ZbControl *c = g_new0 (ZbControl, 1);

    c->key       = key;
    c->kind      = kind;
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

/* (g) Collect the controls on @page_controls whose value changed since the
 * last seed/Apply into a Z2M set-payload, publish it to
 * zigbee2mqtt/<name>/set through the hidden sink, and adopt the applied
 * values as the new baseline so a second press resends nothing. */
static void
zb_publish_changes (ZbDevCtx *ctx, GPtrArray *page_controls)
{
    JsonObject *obj;
    JsonNode   *node;
    PnMessage  *msg;
    gchar      *topic;
    guint       i, changed = 0;

    if (ctx->sink == NULL)
    {
        pn_device_dialog_set_status (
                ctx->shell, "Pick a broker and press Apply before writing.");
        return;
    }
    if (ctx->selected_name == NULL)
        return;

    obj = json_object_new ();
    for (i = 0; i < page_controls->len; i++)
    {
        ZbControl *c   = g_ptr_array_index (page_controls, i);
        gchar     *cur = zb_control_read_string (c);

        if (c->baseline == NULL || g_strcmp0 (cur, c->baseline) != 0)
        {
            zb_control_set_member (obj, c);
            g_free (c->baseline);
            c->baseline = cur;     /* adopt the applied value */
            changed++;
        }
        else
        {
            g_free (cur);
        }
    }

    if (changed == 0)
    {
        json_object_unref (obj);
        pn_device_dialog_set_statusf (ctx->shell, "%s — no changes to apply.",
                                      ctx->selected_name);
        return;
    }

    topic = g_strdup_printf ("%s%s/set", ZB_DEV_TOPIC_PREFIX,
                             ctx->selected_name);
    msg  = pn_message_new (NULL, topic);
    node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (node, obj);          /* transfers obj */
    pn_message_set_member (msg, "payload", node);   /* transfers node */

    pn_node_receive_message (PN_NODE (ctx->sink), msg);
    g_object_unref (msg);

    pn_device_dialog_set_statusf (ctx->shell, "Applied %u change%s to %s.",
                                  changed, changed == 1 ? "" : "s",
                                  ctx->selected_name);
    g_free (topic);
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

/* (e) Map one leaf expose to a row at *@row of @grid, honouring the
 * access bitmap: a settable expose (bit 2) gets its typed editable
 * control, anything else a read-only value row.  An unknown type also
 * falls back to a read-only raw-value row so a newer Z2M expose surfaces
 * rather than vanishing.  The control is tracked under its resolved key
 * for seeding.  A settable control is also filed under @page_controls
 * (borrowed; may be NULL) so the page's Apply can publish it. */
static void
zb_build_expose_row (ZbDevCtx *ctx, GtkGrid *grid, gint *row,
                     JsonObject *exp, GPtrArray *page_controls)
{
    const gchar *type     = zb_peek_string (exp, "type");
    const gchar *label    = zb_expose_label (exp);
    const gchar *desc     = zb_peek_string (exp, "description");
    gboolean     settable = (zb_expose_access (exp) & 2) != 0;
    gchar       *key      = zb_expose_key (exp);
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
        const gchar *unit = zb_peek_string (exp, "unit");
        gint         digits = 0;
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

    if (key != NULL)
    {
        ZbControl *c = zb_track_control (ctx, key, kind, w,
                                         value_on, value_off);
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

/* (d) Build one section of rows from a `features` array into @inner.
 * Leaf features become rows in a single grid titled @title; a nested
 * composite feature (one that itself has `features`) recurses into its
 * own sub-section.  No section is added when the grid stays empty. */
static void
zb_add_features_section (ZbDevCtx *ctx, GtkWidget *inner,
                         const gchar *title, JsonArray *features,
                         GPtrArray *page_controls)
{
    GtkWidget *grid;
    GPtrArray *nested;
    guint      n, i;
    gint       row = 0;

    if (features == NULL)
        return;

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    g_object_ref_sink (grid);

    nested = g_ptr_array_new ();
    n = json_array_get_length (features);
    for (i = 0; i < n; i++)
    {
        JsonNode   *elem = json_array_get_element (features, i);
        JsonObject *exp;

        if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
            continue;
        exp = json_node_get_object (elem);
        if (zb_object_array (exp, "features") != NULL)
            g_ptr_array_add (nested, exp);   /* composite -> sub-section */
        else
            zb_build_expose_row (ctx, GTK_GRID (grid), &row, exp,
                                 page_controls);
    }

    if (row > 0)
        pn_device_form_add_section (inner, title, grid);   /* takes the ref */
    else
        gtk_widget_destroy (grid);
    g_object_unref (grid);

    for (i = 0; i < nested->len; i++)
    {
        JsonObject *exp = g_ptr_array_index (nested, i);
        zb_add_features_section (ctx, inner, zb_expose_label (exp),
                                 zb_object_array (exp, "features"),
                                 page_controls);
    }
    g_ptr_array_unref (nested);
}

/* (d) A top-level composite/typed expose (switch/light/cover/… or
 * "composite") becomes its own notebook page built from its features. */
static void
zb_build_composite_page (ZbDevCtx *ctx, JsonObject *exp)
{
    GtkWidget   *inner;
    GtkWidget   *tab   = pn_device_form_new_tab (&inner);
    const gchar *title = zb_expose_label (exp);
    GPtrArray   *page  = g_ptr_array_new ();   /* borrowed settable controls */

    zb_add_features_section (ctx, inner, title,
                             zb_object_array (exp, "features"), page);
    zb_add_page_apply (ctx, inner, page);      /* (g) consumes page */
    pn_device_dialog_append_page (ctx->shell, tab, title);
    g_ptr_array_add (ctx->device_pages, tab);
    gtk_widget_show_all (tab);
}

/* (d) Walk the whole exposes tree of the selected device into pages:
 * each top-level composite/typed expose gets its own page; the bare
 * top-level exposes (binary/numeric/enum/text/…) collect under a single
 * "Settings" page.  Then jump to the first device page. */
static void
zb_build_device_pages (ZbDevCtx *ctx, JsonArray *exposes)
{
    GPtrArray *bare = g_ptr_array_new ();
    guint      n, i;

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
            if (zb_object_array (exp, "features") != NULL)
                zb_build_composite_page (ctx, exp);
            else
                g_ptr_array_add (bare, exp);
        }
    }

    if (bare->len > 0)
    {
        GtkWidget *inner;
        GtkWidget *tab  = pn_device_form_new_tab (&inner);
        GtkWidget *grid = gtk_grid_new ();
        GPtrArray *page = g_ptr_array_new ();   /* borrowed settable controls */
        gint       row  = 0;

        gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
        gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
        for (i = 0; i < bare->len; i++)
            zb_build_expose_row (ctx, GTK_GRID (grid), &row,
                                 g_ptr_array_index (bare, i), page);

        pn_device_form_add_section (inner, "Settings", grid);
        zb_add_page_apply (ctx, inner, page);   /* (g) consumes page */
        pn_device_dialog_append_page (ctx->shell, tab, "Settings");
        g_ptr_array_add (ctx->device_pages, tab);
        gtk_widget_show_all (tab);
    }
    g_ptr_array_unref (bare);

    /* The static "Broker" page is index 0; show the first device page. */
    if (ctx->device_pages->len > 0)
        pn_device_dialog_set_current_page (ctx->shell, 1);
}

/* (f) Seed every built control from the captured device-state object by
 * its resolved key, leaving the build-time default (or the "—"
 * placeholder on read-only rows) where the value is absent. */
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

    /* (d, e) walk the exposes tree into editable settings pages, then
     * (f) seed every built control from the captured live state and (g)
     * snapshot baselines so each page's Apply publishes only changes.
     * Options still get only the status note below (their own page is
     * item i). */
    zb_build_device_pages (ctx, exposes);
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
