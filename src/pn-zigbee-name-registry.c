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

#include "pn-zigbee-name-registry.h"

#include <pn-mqtt.h>
#include <pn-mqtt-profile.h>
#include <pn-vault.h>
#include <pn-message.h>

/* The retained Zigbee2MQTT inventory topic.  Every install publishes its
 * device list here (a JSON array of device objects, each with a
 * `friendly_name`), so a narrow subscription to just this one topic is all
 * the background collector needs -- no `zigbee2mqtt/#` firehose. */
#define ZB_REG_DEVICES_TOPIC "zigbee2mqtt/bridge/devices"

/* ------------------------------------------------------------------ */
/*  The singleton registry object                                      */
/*                                                                     */
/*  A minimal #GObject so we can hang a "changed" signal off it -- an   */
/*  editor combo built before the first bridge/devices payload arrives  */
/*  connects to it (g_signal_connect_object) and repopulates live when  */
/*  names appear or a device is (un)paired.                             */
/* ------------------------------------------------------------------ */

#define ZB_TYPE_NAME_REGISTRY (zb_name_registry_get_type ())
G_DECLARE_FINAL_TYPE (ZbNameRegistry, zb_name_registry, ZB, NAME_REGISTRY,
                      GObject)

struct _ZbNameRegistry
{
    GObject     parent_instance;

    /* Known devices: owned friendly-name key -> owned #ZbDeviceInfo value.
     * Merged across every broker, so a name is present iff at least one
     * broker reports it. */
    GHashTable *devices;

    /* One hidden #PnMqtt source per broker profile, owned for the process
     * lifetime (the registry is never torn down). */
    GPtrArray  *sources;

    gboolean    started;
};

G_DEFINE_TYPE (ZbNameRegistry, zb_name_registry, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* Per-device record kept in the `devices` table.  @type is the
 * human-readable description (combo's second line; "" until known); @category
 * is the one-word device class ("plug", "leak", "remote", ...) the combo maps
 * to a type glyph. */
typedef struct
{
    gchar *type;
    gchar *category;
} ZbDeviceInfo;

static void
device_info_free (gpointer data)
{
    ZbDeviceInfo *info = data;

    if (info == NULL)
        return;
    g_free (info->type);
    g_free (info->category);
    g_free (info);
}

static void
zb_name_registry_finalize (GObject *object)
{
    ZbNameRegistry *self = ZB_NAME_REGISTRY (object);

    g_clear_pointer (&self->devices, g_hash_table_unref);
    g_clear_pointer (&self->sources, g_ptr_array_unref);

    G_OBJECT_CLASS (zb_name_registry_parent_class)->finalize (object);
}

static void
zb_name_registry_class_init (ZbNameRegistryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = zb_name_registry_finalize;

    /* Fired whenever the merged name set changes. */
    signals[SIG_CHANGED] = g_signal_new (
            "changed", G_TYPE_FROM_CLASS (klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
            G_TYPE_NONE, 0);
}

static void
zb_name_registry_init (ZbNameRegistry *self)
{
    self->devices = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, device_info_free);
    self->sources = g_ptr_array_new_with_free_func (g_object_unref);
    self->started = FALSE;
}

/* Process-global instance, created on first use. */
static ZbNameRegistry *
registry_get (void)
{
    static ZbNameRegistry *instance;

    if (instance == NULL)
        instance = g_object_new (ZB_TYPE_NAME_REGISTRY, NULL);
    return instance;
}

/* ------------------------------------------------------------------ */
/*  Ingest                                                             */
/* ------------------------------------------------------------------ */

/* Borrow a string-valued member off @obj, or NULL when absent / not a
 * string.  Same shape as pn-zigbee-gui.c's zb_peek_string(). */
static const gchar *
peek_string (JsonObject *obj, const gchar *key)
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

/* Borrow an object-valued member off @obj, or NULL when absent / not an
 * object. */
static JsonObject *
peek_object (JsonObject *obj, const gchar *key)
{
    JsonNode *n;

    if (obj == NULL || !json_object_has_member (obj, key))
        return NULL;
    n = json_object_get_member (obj, key);
    if (n == NULL || !JSON_NODE_HOLDS_OBJECT (n))
        return NULL;
    return json_node_get_object (n);
}

/* The human-readable type for a device object: the nested
 * definition.description ("Water leak detector", "Security remote control"),
 * falling back to the top-level `type` ("Coordinator" / "Router" /
 * "EndDevice") when there is no definition.  Returns a borrowed string (valid
 * while @dev lives) or NULL when neither is present. */
static const gchar *
device_type (JsonObject *dev)
{
    JsonObject  *def  = peek_object (dev, "definition");
    const gchar *desc = peek_string (def, "description");

    if (desc != NULL && *desc != '\0')
        return desc;
    return peek_string (dev, "type");
}

/* Does @expose (or any of its nested `features`) carry the ZCL "settable"
 * access bit (access & 2)?  Mirror of pn-zigbee-source.c's
 * expose_has_settable(): compound exposes (switch/light/climate) hold their
 * state under `features`, so the walk recurses. */
static gboolean
expose_has_settable (JsonObject *expose)
{
    if (expose == NULL)
        return FALSE;

    if (json_object_has_member (expose, "access"))
    {
        JsonNode *n = json_object_get_member (expose, "access");
        if (n != NULL && JSON_NODE_HOLDS_VALUE (n))
        {
            GType t = json_node_get_value_type (n);
            if ((t == G_TYPE_INT64 || t == G_TYPE_DOUBLE) &&
                (json_node_get_int (n) & 0x2) != 0)
                return TRUE;
        }
    }

    if (json_object_has_member (expose, "features"))
    {
        JsonNode *fn = json_object_get_member (expose, "features");
        if (fn != NULL && JSON_NODE_HOLDS_ARRAY (fn))
        {
            JsonArray *fa = json_node_get_array (fn);
            guint      m  = json_array_get_length (fa);
            guint      i;
            for (i = 0; i < m; i++)
            {
                JsonNode *e = json_array_get_element (fa, i);
                if (e != NULL && JSON_NODE_HOLDS_OBJECT (e) &&
                    expose_has_settable (json_node_get_object (e)))
                    return TRUE;
            }
        }
    }

    return FALSE;
}

/* One-word device class the combo maps to a type glyph.  Mirrors
 * synthesize_category() in pn-zigbee-source.c (canonical expose type ->
 * light / switch->plug-when-mains / lock / cover / fan / climate; else an
 * `action` expose -> remote; else all-read-only -> sensor; else device), with
 * two refinements matching the node set: the network Coordinator, and a
 * water-leak sensor split out of the generic `sensor` bucket so it can show
 * the water-drop glyph.  Never returns NULL (at worst "device"). */
static const gchar *
device_category (JsonObject *dev)
{
    JsonObject  *def;
    JsonNode    *exposes_node;
    JsonArray   *exposes;
    const gchar *power_source;
    const gchar *toptype;
    gboolean     is_mains, has_action, has_leak, all_read_only;
    guint        n, i;

    toptype = peek_string (dev, "type");
    if (toptype != NULL && g_strcmp0 (toptype, "Coordinator") == 0)
        return "coordinator";

    def = peek_object (dev, "definition");
    if (def == NULL || !json_object_has_member (def, "exposes"))
        return "device";
    exposes_node = json_object_get_member (def, "exposes");
    if (exposes_node == NULL || !JSON_NODE_HOLDS_ARRAY (exposes_node))
        return "device";
    exposes = json_node_get_array (exposes_node);
    n       = json_array_get_length (exposes);
    if (n == 0)
        return "device";

    power_source = peek_string (dev, "power_source");
    is_mains     = (power_source != NULL &&
                    g_str_has_prefix (power_source, "Mains"));
    has_action    = FALSE;
    has_leak      = FALSE;
    all_read_only = TRUE;

    for (i = 0; i < n; i++)
    {
        JsonNode    *elem = json_array_get_element (exposes, i);
        JsonObject  *eo;
        const gchar *type;
        const gchar *prop;

        if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
            continue;
        eo   = json_node_get_object (elem);
        type = peek_string (eo, "type");

        if (type != NULL)
        {
            if (g_strcmp0 (type, "light")   == 0) return "light";
            if (g_strcmp0 (type, "switch")  == 0) return is_mains ? "plug" : "switch";
            if (g_strcmp0 (type, "lock")    == 0) return "lock";
            if (g_strcmp0 (type, "cover")   == 0) return "cover";
            if (g_strcmp0 (type, "fan")     == 0) return "fan";
            if (g_strcmp0 (type, "climate") == 0) return "climate";
        }

        prop = peek_string (eo, "property");
        if (prop != NULL)
        {
            if (g_strcmp0 (prop, "action")     == 0) has_action = TRUE;
            if (g_strcmp0 (prop, "water_leak") == 0) has_leak   = TRUE;
        }

        if (expose_has_settable (eo))
            all_read_only = FALSE;
    }

    if (has_action)    return "remote";
    if (has_leak)      return "leak";
    if (all_read_only) return "sensor";
    return "device";
}

void
zb_name_registry_ingest_devices (JsonNode *payload)
{
    ZbNameRegistry *self = registry_get ();
    JsonArray      *arr;
    guint           n, i;
    gboolean        changed = FALSE;

    /* Ignore a malformed publish -- never blank a good set.  bridge/devices
     * is retained, so the last good array stays authoritative. */
    if (payload == NULL || !JSON_NODE_HOLDS_ARRAY (payload))
        return;
    arr = json_node_get_array (payload);
    if (arr == NULL)
        return;

    /* bridge/devices is the complete inventory for its broker, but we only
     * ever grow the merged set here: a name reported by any broker stays
     * offered until the editor restarts.  The overwhelmingly common single-
     * broker setup is exact; a device removed mid-session simply lingers in
     * the dropdown (still typeable, harmless) until the next launch.  A
     * future refinement can track per-broker sets and prune on removal. */
    n = json_array_get_length (arr);
    for (i = 0; i < n; i++)
    {
        JsonNode     *elem = json_array_get_element (arr, i);
        JsonObject   *dev;
        const gchar  *fname;
        const gchar  *type;
        const gchar  *cat;
        ZbDeviceInfo *info;

        if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
            continue;
        dev   = json_node_get_object (elem);
        fname = peek_string (dev, "friendly_name");
        if (fname == NULL || *fname == '\0')
            continue;
        type = device_type (dev);       /* may be NULL */
        cat  = device_category (dev);    /* never NULL (>= "device") */

        info = g_hash_table_lookup (self->devices, fname);
        if (info == NULL)
        {
            info           = g_new0 (ZbDeviceInfo, 1);
            info->type     = g_strdup (type != NULL ? type : "");
            info->category = g_strdup (cat);
            g_hash_table_insert (self->devices, g_strdup (fname), info);
            changed = TRUE;
        }
        else
        {
            /* Known name: only fill a field we did not have yet, so a later
             * fallback publish (no definition) never clobbers a good
             * description or a specific category with the "device" default. */
            if ((info->type == NULL || *info->type == '\0') &&
                type != NULL && *type != '\0')
            {
                g_free (info->type);
                info->type = g_strdup (type);
                changed = TRUE;
            }
            if ((info->category == NULL || *info->category == '\0' ||
                 g_strcmp0 (info->category, "device") == 0) &&
                g_strcmp0 (cat, "device") != 0)
            {
                g_free (info->category);
                info->category = g_strdup (cat);
                changed = TRUE;
            }
        }
    }

    if (changed)
        g_signal_emit (self, signals[SIG_CHANGED], 0);
}

/* "message" handler for every hidden source.  PnMqtt marshals each PUBLISH
 * onto the main thread before emitting, so touching the shared set (and,
 * downstream, GTK combos via the "changed" signal) is safe here. */
static void
on_devices_msg (PnMqtt *source, PnMessage *message, gpointer user_data)
{
    const gchar *topic;

    (void) source;
    (void) user_data;

    topic = pn_message_get_topic (message);
    if (g_strcmp0 (topic, ZB_REG_DEVICES_TOPIC) != 0)
        return;

    zb_name_registry_ingest_devices (
            pn_message_get_member (message, "payload"));
}

/* Open one hidden source subscribed to bridge/devices on @profile_id
 * ("" follows the primary/default broker).  Mirrors zb_start_source() in
 * pn-zigbee-gui.c but with a narrow subscription and no write-back sink. */
static void
start_source_for (ZbNameRegistry *self, const gchar *profile_id)
{
    PnMqtt *source = pn_mqtt_new ();

    g_object_set (source,
                  "broker-profile",  profile_id,
                  "subscribe-topic", ZB_REG_DEVICES_TOPIC,
                  NULL);
    g_signal_connect (source, "message",
                      G_CALLBACK (on_devices_msg), self);

    /* Setting the properties schedules the connect on the next main-loop
     * idle (PnMqtt debounces it); the retained bridge/devices then arrives
     * on its own.  Owned for the process lifetime. */
    g_ptr_array_add (self->sources, source);
}

/* ------------------------------------------------------------------ */
/*  Public: start / snapshot / signal source                          */
/* ------------------------------------------------------------------ */

void
zb_name_registry_start (void)
{
    ZbNameRegistry *self = registry_get ();
    PnVault        *vault;
    GList          *profiles, *l;

    if (self->started)
        return;
    self->started = TRUE;

    vault = pn_vault_get_default ();
    if (vault == NULL)
        return;

    /* One narrow subscription per configured broker; the sets are merged so
     * a name reported by any broker shows up.  (transfer container: the
     * PnProfile elements are borrowed from the vault.) */
    profiles = pn_vault_list_profiles (vault, PN_PROFILE_TYPE_MQTT_BROKER);
    for (l = profiles; l != NULL; l = l->next)
    {
        PnProfile   *p  = l->data;
        const gchar *id = pn_profile_get_id (p);

        start_source_for (self, id != NULL ? id : "");
    }
    g_list_free (profiles);

    /* No explicit profiles: still try the default broker (empty id follows
     * whatever the primary resolves to; if none, the connect simply fails
     * silently and the set stays empty). */
    if (self->sources->len == 0)
        start_source_for (self, "");
}

static gint
cmp_names (gconstpointer a, gconstpointer b)
{
    const gchar *sa = *(const gchar * const *) a;
    const gchar *sb = *(const gchar * const *) b;
    return g_strcmp0 (sa, sb);
}

GPtrArray *
zb_name_registry_get_names (void)
{
    ZbNameRegistry *self = registry_get ();
    GPtrArray      *out  = g_ptr_array_new_with_free_func (g_free);
    GHashTableIter  iter;
    gpointer        key;

    g_hash_table_iter_init (&iter, self->devices);
    while (g_hash_table_iter_next (&iter, &key, NULL))
        g_ptr_array_add (out, g_strdup (key));

    g_ptr_array_sort (out, cmp_names);
    return out;
}

gchar *
zb_name_registry_lookup_type (const gchar *name)
{
    ZbNameRegistry *self = registry_get ();
    ZbDeviceInfo   *info;

    if (name == NULL)
        return NULL;
    info = g_hash_table_lookup (self->devices, name);
    return (info != NULL && info->type != NULL && *info->type != '\0')
           ? g_strdup (info->type) : NULL;
}

gchar *
zb_name_registry_lookup_category (const gchar *name)
{
    ZbNameRegistry *self = registry_get ();
    ZbDeviceInfo   *info;

    if (name == NULL)
        return NULL;
    info = g_hash_table_lookup (self->devices, name);
    return (info != NULL && info->category != NULL && *info->category != '\0')
           ? g_strdup (info->category) : NULL;
}

GObject *
zb_name_registry_get_object (void)
{
    return G_OBJECT (registry_get ());
}
