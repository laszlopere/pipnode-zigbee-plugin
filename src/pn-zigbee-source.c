/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 *
 * This file is part of the pipnode-zigbee-plugin, a proprietary plugin
 * for Pipnode.  It links the pipnode host library solely through the
 * documented plugin interface (pn_plugin_init and the public pipnode
 * headers) and is therefore distributed under the additional permission
 * granted by Pipnode's LICENSE.PLUGIN-EXCEPTION, which allows a plugin
 * to be released under any license, including this proprietary one.
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-zigbee-source.h"

#include <pn-message.h>

#include <json-glib/json-glib.h>
#include <string.h>

/* fa-rss U+F09E -- same glyph the base #PnMqtt uses, since this is the
 * same role (an MQTT subscriber) just preconfigured for the Z2M topic
 * tree.  No "needs configuration" warning glyph here: a freshly-dropped
 * node already has a usable subscribe filter (`zigbee2mqtt/#`), so the
 * red-❗ "fill me in" cue PnZigbeeSwitch uses would be misleading. */
#define PN_ZIGBEE_SOURCE_ICON "\xef\x82\x9e"

/* Z2M's fixed topic conventions.  Kept as string literals rather than
 * properties because the names are dictated by the Z2M wire contract;
 * users who want different exclusions narrow the subscribe-filter
 * directly. */
#define PN_ZIGBEE_BASE_TOPIC        "zigbee2mqtt"
#define PN_ZIGBEE_LOGGING_TOPIC     "zigbee2mqtt/bridge/logging"
#define PN_ZIGBEE_DEVICES_TOPIC     "zigbee2mqtt/bridge/devices"
#define PN_ZIGBEE_DEFINITIONS_TOPIC "zigbee2mqtt/bridge/definitions"
#define PN_ZIGBEE_INFO_TOPIC        "zigbee2mqtt/bridge/info"
#define PN_ZIGBEE_BRIDGE_PREFIX     "zigbee2mqtt/bridge/"

/* Per-device cache entry.  Two precomputed JsonNodes carrying
 * non-overlapping field sets, so each maps cleanly to one of the
 * inject-* properties and combinations land in the same shape
 * regardless of which flags drove them:
 *
 *   `info`         -- the Z2M `include_device_information`-shaped
 *                     identity scalars (friendlyName, ieeeAddr,
 *                     networkAddress, type, powerSource,
 *                     manufacturerName, modelID, model, dateCode,
 *                     softwareBuildID).  Always cheap; ~200 B per
 *                     device.
 *
 *   `capabilities` -- the synthesised category plus the
 *                     description / vendor / supportsOta scalars
 *                     and the structured exposes / options trees
 *                     from Z2M's definition.  Several KB per
 *                     device for richly-featured ones.  %NULL for
 *                     devices with neither -- in practice always
 *                     present, since synthesize_category() falls
 *                     back to "device" even without a definition.
 *
 * Caching both as separate objects lets injection union them at
 * splice time based on the live property flags, so toggling either
 * property at runtime takes effect on the next publish without
 * rebuilding the cache or re-traversing the source definition. */
typedef struct
{
    JsonNode *info;
    JsonNode *capabilities;
} ZigbeeCachedDevice;

struct _PnZigbeeSource
{
    PnMqtt parent_instance;

    /* Whether accept_topic should drop `zigbee2mqtt/bridge/logging` on
     * the network thread.  Cross-thread access (set on the main
     * thread, read on libmosquitto's network thread) is handled
     * through g_atomic_int_get/set so the read is portable and the
     * write is publishing-safe -- the property dialog may flip this
     * at any time while the broker connection is live. */
    gint filter_logging;

    /* Whether process_message should swallow the bulky bridge envelopes
     * (`bridge/devices`, `bridge/definitions`, `bridge/info`) after
     * consuming them internally.  Unlike `bridge/logging` -- which is
     * cheap to drop early in accept_topic because the node never needs
     * to look at it -- these three carry data the node itself wants
     * (devices feeds the device-info cache, and definitions/info may
     * grow internal consumers later), so the filter has to live on the
     * main-thread late hook: parse first, consume internally, then
     * suppress the downstream emit by returning FALSE from
     * process_message.  Main-thread only, like the inject_* flags. */
    gboolean filter_bridge_devices;
    gboolean filter_bridge_definitions;
    gboolean filter_bridge_info;

    /* Whether process_message should graft a `device` block onto each
     * per-device state publish's `data.payload` at all.  Main-thread
     * only -- the property dialog and the process_message vfunc both
     * run on the main thread, so a plain gboolean suffices. */
    gboolean inject_device_info;

    /* When TRUE, the injected `device` block includes the
     * capability/description/vendor/supportsOta/category/exposes/
     * options extension fields on top of the Z2M-compatible base
     * shape.  Off by default because the `exposes` and `options`
     * trees can be several KB per device, making every state
     * publish much bigger.  No effect when inject_device_info is
     * FALSE. */
    gboolean inject_device_capabilities;

    /* Cache populated from each `zigbee2mqtt/bridge/devices` publish:
     * friendly_name (gchar *, owned) -> ZigbeeCachedDevice (owned)
     * carrying the two precomputed info blocks.  Touched only on the
     * main thread.  Cleared and rebuilt wholesale on every bridge/
     * devices receipt -- Z2M republishes the topic whenever the
     * device set changes and the broker delivers it retained on
     * reconnect, so the cache stays in sync without a separate
     * invalidation path. */
    GHashTable *device_info;
};

G_DEFINE_TYPE (PnZigbeeSource, pn_zigbee_source, PN_TYPE_MQTT)

enum {
    PROP_0,
    PROP_FILTER_LOGGING,
    PROP_FILTER_BRIDGE_DEVICES,
    PROP_FILTER_BRIDGE_DEFINITIONS,
    PROP_FILTER_BRIDGE_INFO,
    PROP_INJECT_DEVICE_INFO,
    PROP_INJECT_DEVICE_CAPABILITIES,
    N_PROPS,
};

static void
zigbee_cached_device_free (gpointer data)
{
    ZigbeeCachedDevice *c = data;
    if (c == NULL)
        return;
    g_clear_pointer (&c->info,         json_node_unref);
    g_clear_pointer (&c->capabilities, json_node_unref);
    g_free (c);
}

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Early topic filter                                                 */
/* ------------------------------------------------------------------ */

/** Override of #PnMqttClass::accept_topic.  Runs on libmosquitto's
 *  network thread for every inbound PUBLISH, BEFORE the base builds a
 *  #PnMessage -- so a rejected publish costs nothing more than this
 *  string compare.  The only topic dropped here is
 *  `zigbee2mqtt/bridge/logging` (Z2M's log-mirror channel), and only
 *  when #PnZigbeeSource:filter-logging is enabled.  Everything else
 *  delegates to the parent default (which currently accepts
 *  unconditionally) -- chaining instead of returning TRUE outright
 *  stays future-proof against the base default gaining real
 *  behaviour later. */
static gboolean
pn_zigbee_source_accept_topic (
        PnMqtt      *base,
        const gchar *topic)
{
    PnZigbeeSource *self = PN_ZIGBEE_SOURCE (base);

    /* g_atomic_int_get for the read because the field is mutated on
     * the main thread (property dialog) and read here on the network
     * thread.  Cheap on x86/ARM64 (single aligned word load) but the
     * atomic call documents the cross-thread contract and stays
     * correct on weaker memory models. */
    if (g_atomic_int_get (&self->filter_logging) &&
        topic != NULL &&
        strcmp (topic, PN_ZIGBEE_LOGGING_TOPIC) == 0)
        return FALSE;

    return pn_mqtt_real_accept_topic (base, topic);
}

/* ------------------------------------------------------------------ */
/*  Device-info cache + injection                                      */
/* ------------------------------------------------------------------ */

/** Read a string-valued member off @src and write it to @dst under a
 *  (possibly different) key, mapping Z2M's bridge/devices snake_case
 *  fields onto the camelCase shape its include_device_information
 *  output uses.  Missing / non-string members are skipped silently so
 *  partial device records (e.g. the Coordinator entry, which has no
 *  power_source / date_code) yield a partial info block rather than
 *  failing the whole cache write. */
static void
copy_string_field (
        JsonObject  *dst,
        const gchar *dst_key,
        JsonObject  *src,
        const gchar *src_key)
{
    JsonNode *n;

    if (!json_object_has_member (src, src_key))
        return;
    n = json_object_get_member (src, src_key);
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
        return;
    if (json_node_get_value_type (n) != G_TYPE_STRING)
        return;
    json_object_set_string_member (dst, dst_key, json_node_get_string (n));
}

/** Same idea as copy_string_field but for integer-valued members.
 *  Z2M's bridge/devices uses JSON numbers (network_address is the only
 *  one we currently care about); accept both INT64 and DOUBLE storage
 *  to be tolerant of generator differences. */
static void
copy_int_field (
        JsonObject  *dst,
        const gchar *dst_key,
        JsonObject  *src,
        const gchar *src_key)
{
    JsonNode *n;
    GType     t;

    if (!json_object_has_member (src, src_key))
        return;
    n = json_object_get_member (src, src_key);
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
        return;
    t = json_node_get_value_type (n);
    if (t != G_TYPE_INT64 && t != G_TYPE_DOUBLE)
        return;
    json_object_set_int_member (dst, dst_key, json_node_get_int (n));
}

/** Same idea for boolean-valued members.  Used for `supports_ota`. */
static void
copy_bool_field (
        JsonObject  *dst,
        const gchar *dst_key,
        JsonObject  *src,
        const gchar *src_key)
{
    JsonNode *n;

    if (!json_object_has_member (src, src_key))
        return;
    n = json_object_get_member (src, src_key);
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
        return;
    if (json_node_get_value_type (n) != G_TYPE_BOOLEAN)
        return;
    json_object_set_boolean_member (dst, dst_key, json_node_get_boolean (n));
}

/** Deep-copy a JsonNode member from @src to @dst, preserving its
 *  structure verbatim.  Used for the `exposes` / `options` capability
 *  trees: those are nested arrays of objects that downstream code
 *  wants to traverse, not stringify, so we hand over a real subtree
 *  rather than a serialised form.  The copy means a downstream mutation
 *  cannot leak back into the cache. */
static void
copy_member_deep (
        JsonObject  *dst,
        const gchar *dst_key,
        JsonObject  *src,
        const gchar *src_key)
{
    JsonNode *n;

    if (!json_object_has_member (src, src_key))
        return;
    n = json_object_get_member (src, src_key);
    if (n == NULL)
        return;
    json_object_set_member (dst, dst_key, json_node_copy (n));
}

/** Read a string-valued member off a JSON object without taking
 *  ownership.  Returns a borrowed pointer (valid for as long as the
 *  object stays alive) or %NULL when the member is missing or not a
 *  string.  Used in the category synthesiser, where we just need to
 *  peek at "type" / "property" strings without copying anything. */
static const gchar *
peek_string_member (
        JsonObject  *obj,
        const gchar *key)
{
    JsonNode *n;

    if (obj == NULL || !json_object_has_member (obj, key))
        return NULL;
    n = json_object_get_member (obj, key);
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
        return NULL;
    if (json_node_get_value_type (n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string (n);
}

/** Recursive predicate: does any expose (or any of its nested
 *  features) carry the ZCL "settable" access bit?  Z2M encodes the
 *  bitmap as: 1=published, 2=settable, 4=gettable -- so the test
 *  here is `access & 2`.  Compound exposes (switch/light/climate)
 *  carry their state under a `features` sub-array rather than a
 *  top-level access flag, so the walk has to recurse through
 *  features too; without that an "all read-only" check would falsely
 *  classify a switch as a sensor. */
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
            if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE)
            {
                if ((json_node_get_int (n) & 0x2) != 0)
                    return TRUE;
            }
        }
    }

    if (json_object_has_member (expose, "features"))
    {
        JsonNode *fn = json_object_get_member (expose, "features");
        if (fn != NULL && JSON_NODE_HOLDS_ARRAY (fn))
        {
            JsonArray *fa = json_node_get_array (fn);
            guint      n  = json_array_get_length (fa);
            guint      i;
            for (i = 0; i < n; i++)
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

/** Best-effort device-class label from the exposes tree + power
 *  source.  Returned strings are static literals (no ownership
 *  transfer).  Ordering of the heuristic:
 *
 *    1. Walk the top-level exposes once, looking for one of the
 *       canonical Z2M device types (light / switch / lock / cover /
 *       fan / climate).  First hit wins.  A "switch" expose on a
 *       mains-powered device is reported as "plug" to match the
 *       intuition of "TS011F = smart plug", not "TS011F = switch"
 *       (Z2M itself does not distinguish the two; we do because the
 *       user-visible label is more informative).
 *
 *    2. If no device type matched but any top-level expose has
 *       property="action", treat the device as a "remote" (Z2M's
 *       convention for scene controllers / IAS Zone remotes / push
 *       buttons -- the TS0215A panic button is exactly this shape).
 *
 *    3. If every top-level expose is read-only (no `access & 2`),
 *       it's a "sensor".
 *
 *    4. Fallback "device" when none of the above fits (e.g. devices
 *       with no definition at all, like the Coordinator entry). */
static const gchar *
synthesize_category (
        JsonObject *device,
        JsonObject *definition)
{
    JsonNode    *exposes_node;
    JsonArray   *exposes;
    const gchar *power_source;
    gboolean     is_mains;
    gboolean     has_action;
    gboolean     all_read_only;
    guint        n, i;

    if (definition == NULL ||
        !json_object_has_member (definition, "exposes"))
        return "device";

    exposes_node = json_object_get_member (definition, "exposes");
    if (exposes_node == NULL || !JSON_NODE_HOLDS_ARRAY (exposes_node))
        return "device";

    exposes = json_node_get_array (exposes_node);
    n       = json_array_get_length (exposes);
    if (n == 0)
        return "device";

    power_source = peek_string_member (device, "power_source");
    is_mains     = (power_source != NULL &&
                    g_str_has_prefix (power_source, "Mains"));

    has_action    = FALSE;
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
        type = peek_string_member (eo, "type");

        if (type != NULL)
        {
            if (g_strcmp0 (type, "light")   == 0) return "light";
            if (g_strcmp0 (type, "switch")  == 0) return is_mains ? "plug" : "switch";
            if (g_strcmp0 (type, "lock")    == 0) return "lock";
            if (g_strcmp0 (type, "cover")   == 0) return "cover";
            if (g_strcmp0 (type, "fan")     == 0) return "fan";
            if (g_strcmp0 (type, "climate") == 0) return "climate";
        }

        prop = peek_string_member (eo, "property");
        if (prop != NULL && g_strcmp0 (prop, "action") == 0)
            has_action = TRUE;

        if (expose_has_settable (eo))
            all_read_only = FALSE;
    }

    if (has_action)    return "remote";
    if (all_read_only) return "sensor";
    return "device";
}

/** Populate the Z2M `include_device_information`-compatible base
 *  fields (friendlyName .. softwareBuildID, plus `model` from
 *  definition).  Used for both the `basic` cache entry and the
 *  shared prefix of the `full` cache entry, so a Z2M-shape consumer
 *  sees the exact same fields regardless of which entry got
 *  spliced. */
static void
populate_basic_fields (
        JsonObject *info,
        JsonObject *device,
        JsonObject *def)
{
    copy_string_field (info, "friendlyName",     device, "friendly_name");
    copy_string_field (info, "ieeeAddr",         device, "ieee_address");
    copy_int_field    (info, "networkAddress",   device, "network_address");
    copy_string_field (info, "type",             device, "type");
    copy_string_field (info, "powerSource",      device, "power_source");
    copy_string_field (info, "manufacturerName", device, "manufacturer");
    copy_string_field (info, "modelID",          device, "model_id");
    copy_string_field (info, "dateCode",         device, "date_code");
    copy_string_field (info, "softwareBuildID",  device, "software_build_id");

    if (def != NULL)
        copy_string_field (info, "model", def, "model");
}

/** Append the Pipnode extension fields on top of an info object that
 *  already carries the Z2M-compatible base: human-readable
 *  description / vendor / supports_ota, the full structured exposes
 *  + options trees, and a synthesised category.  All derived from
 *  the same `bridge/devices` source the basic fields came from.
 *
 *  Fields not in `bridge/devices` (applicationVersion / stackVersion
 *  / zclVersion / hardwareVersion / manufacturerID -- Z2M sources
 *  these from its in-process zigbee-herdsman device object) are
 *  omitted; the rest of the shape is identical to what Z2M would
 *  publish if it had decided to surface this data alongside
 *  include_device_information. */
static void
populate_capability_fields (
        JsonObject *info,
        JsonObject *device,
        JsonObject *def)
{
    const gchar *category;

    if (def != NULL)
    {
        copy_string_field (info, "description", def, "description");
        copy_string_field (info, "vendor",      def, "vendor");
        copy_bool_field   (info, "supportsOta", def, "supports_ota");
        copy_member_deep  (info, "exposes",     def, "exposes");
        copy_member_deep  (info, "options",     def, "options");
    }

    /* Computed against the raw device + definition objects rather
     * than the already-built info object so the heuristic reads
     * native Z2M shapes (snake_case "power_source", "access" int
     * bitmap, ...) directly. */
    category = synthesize_category (device, def);
    if (category != NULL)
        json_object_set_string_member (info, "category", category);
}

/** Build the two non-overlapping cache nodes for one device in a
 *  single pass, sharing the definition lookup so the bridge/devices
 *  walk stays linear.  Returned struct is owned by the caller and
 *  freed via zigbee_cached_device_free().
 *
 *  The objects carry disjoint key sets by construction
 *  (populate_basic_fields and populate_capability_fields touch no
 *  shared keys), so the injection-side union is just a key-copy
 *  walk -- no per-key conflict resolution needed. */
static ZigbeeCachedDevice *
build_cached_device (JsonObject *device)
{
    ZigbeeCachedDevice *out = g_new0 (ZigbeeCachedDevice, 1);
    JsonObject         *def = NULL;
    JsonObject         *info_obj;
    JsonObject         *caps_obj;

    if (json_object_has_member (device, "definition"))
    {
        JsonNode *def_node = json_object_get_member (device, "definition");
        if (def_node != NULL && JSON_NODE_HOLDS_OBJECT (def_node))
            def = json_node_get_object (def_node);
    }

    info_obj = json_object_new ();
    populate_basic_fields (info_obj, device, def);
    out->info = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (out->info, info_obj);

    caps_obj = json_object_new ();
    populate_capability_fields (caps_obj, device, def);
    out->capabilities = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (out->capabilities, caps_obj);

    return out;
}

/** Copy every direct member of @src into @dst.  Deep-copies each
 *  value so @dst owns its own subtree -- callers can hand the
 *  result off to a message without aliasing the cache. */
static void
merge_object_members (
        JsonObject *dst,
        JsonNode   *src_node)
{
    JsonObject *src;
    GList      *members;
    GList      *iter;

    if (src_node == NULL || !JSON_NODE_HOLDS_OBJECT (src_node))
        return;
    src = json_node_get_object (src_node);
    if (src == NULL)
        return;

    members = json_object_get_members (src);
    for (iter = members; iter != NULL; iter = iter->next)
    {
        const gchar *key = iter->data;
        JsonNode    *val = json_object_get_member (src, key);
        if (val != NULL)
            json_object_set_member (dst, key, json_node_copy (val));
    }
    g_list_free (members);
}

/** Drain the bridge/devices payload (a top-level JSON array of device
 *  records) into the cache.  Wholesale rebuild: the table is cleared
 *  first so a device removed in Z2M stops getting decorated here on
 *  the very next publish, no separate invalidation step. */
static void
rebuild_device_cache (
        PnZigbeeSource *self,
        PnMessage      *message)
{
    JsonNode  *payload_node;
    JsonArray *arr;
    guint      n, i;
    guint      skipped = 0;

    payload_node = pn_message_get_member (message, "payload");
    if (payload_node == NULL || !JSON_NODE_HOLDS_ARRAY (payload_node))
    {
        /* Not the array we expect -- bail before clearing the table so
         * the previous inventory survives a malformed publish (PLUGINS
         * §12, channel 3: log instead of dropping silently). */
        pn_node_log_warning (PN_NODE (self),
                             "bridge/devices payload is not a JSON array; "
                             "ignoring (device cache unchanged)");
        return;
    }

    arr = json_node_get_array (payload_node);
    if (arr == NULL)
        return;

    g_hash_table_remove_all (self->device_info);

    n = json_array_get_length (arr);
    for (i = 0; i < n; i++)
    {
        JsonNode           *elem = json_array_get_element (arr, i);
        JsonObject         *dev;
        JsonNode           *fname_node;
        const gchar        *fname;
        ZigbeeCachedDevice *entry;

        if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
        {
            skipped++;
            continue;
        }

        dev = json_node_get_object (elem);
        if (!json_object_has_member (dev, "friendly_name"))
        {
            skipped++;
            continue;
        }

        fname_node = json_object_get_member (dev, "friendly_name");
        if (fname_node == NULL || !JSON_NODE_HOLDS_VALUE (fname_node) ||
            json_node_get_value_type (fname_node) != G_TYPE_STRING)
        {
            skipped++;
            continue;
        }

        fname = json_node_get_string (fname_node);
        if (fname == NULL || *fname == '\0')
        {
            skipped++;
            continue;
        }

        entry = build_cached_device (dev);
        g_hash_table_insert (self->device_info, g_strdup (fname), entry);
    }

    /* One aggregated line rather than per-entry spam: a healthy
     * inventory skips nothing, so this stays quiet in normal operation
     * (PLUGINS §12, channel 3). */
    if (skipped > 0)
        pn_node_log_warning (PN_NODE (self),
                             "bridge/devices: skipped %u entr%s with no "
                             "usable friendly_name",
                             skipped, skipped == 1 ? "y" : "ies");
}

/** Splice a fresh `device` object built from the chosen union of
 *  the cache entry's two field sets into @message's `data.payload`.
 *  Each enabled flag contributes its block's keys independently --
 *  identity scalars from `info`, the synthesised category and
 *  description / vendor / supportsOta / exposes / options from
 *  `capabilities` -- so the four legal flag combinations all
 *  produce the right shape:
 *
 *    info=0, caps=0  -> no `device` member at all (caller skips)
 *    info=1, caps=0  -> Z2M-compatible identity block
 *    info=0, caps=1  -> capability-only block (no identity scalars
 *                       duplicated; the publish's envelope topic
 *                       already names the device)
 *    info=1, caps=1  -> union of both, identity fields first
 *
 *  Walks the cached objects' members and deep-copies each value so
 *  the message owns its own subtree -- a downstream node that
 *  rewrites the payload cannot corrupt the cache, and the cache
 *  entries stay usable for the next state publish.
 *
 *  Bails silently when the payload is not a JSON object (Z2M
 *  occasionally publishes bare-string availability messages); the
 *  state-object case is the only one where the `device` block
 *  makes sense to add. */
static void
inject_device_info_block (
        PnMessage          *message,
        ZigbeeCachedDevice *entry,
        gboolean            want_info,
        gboolean            want_capabilities)
{
    JsonNode   *payload_node;
    JsonObject *payload_obj;
    JsonObject *device_obj;
    JsonNode   *device_node;

    if (!want_info && !want_capabilities)
        return;

    payload_node = pn_message_get_member (message, "payload");
    if (payload_node == NULL || !JSON_NODE_HOLDS_OBJECT (payload_node))
        return;

    payload_obj = json_node_get_object (payload_node);
    if (payload_obj == NULL)
        return;

    device_obj = json_object_new ();
    if (want_info)
        merge_object_members (device_obj, entry->info);
    if (want_capabilities)
        merge_object_members (device_obj, entry->capabilities);

    /* Guard against the both-cached-blocks-empty edge case (devices
     * the cache walk couldn't extract any field from at all).
     * Without this an empty {} would land under `device`, which is
     * worse than no decoration -- the downstream consumer can't
     * tell whether it means "unknown" or "no fields apply". */
    if (json_object_get_size (device_obj) == 0)
    {
        json_object_unref (device_obj);
        return;
    }

    device_node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (device_node, device_obj);
    json_object_set_member (payload_obj, "device", device_node);
}

/** Override of #PnMqttClass::process_message.  Two responsibilities:
 *
 *    1. Side-effect on bridge/devices publishes -- refresh the
 *       per-friendly-name cache so the next per-device publish has a
 *       record to inject from.  We do NOT drop the bridge/devices
 *       envelope itself: a downstream Debug node or a third-party
 *       inventory consumer may legitimately want to see it.
 *
 *    2. Decoration on per-device state publishes -- look the topic's
 *       friendly-name suffix up in the cache; on a hit, splice the
 *       cached info block in under `data.payload.device`.
 *
 *  Chaining to pn_mqtt_real_process_message at the end keeps us
 *  future-proof against the base default growing real behaviour, and
 *  matches the convention the accept_topic override uses. */
static gboolean
pn_zigbee_source_process_message (
        PnMqtt    *base,
        PnMessage *message)
{
    PnZigbeeSource *self = PN_ZIGBEE_SOURCE (base);
    const gchar    *topic;

    topic = pn_message_get_topic (message);
    if (topic == NULL || !g_str_has_prefix (topic, PN_ZIGBEE_BASE_TOPIC "/"))
        return pn_mqtt_real_process_message (base, message);

    /* Always refresh on bridge/devices, even when injection is
     * disabled, so toggling the property TRUE later does not require
     * a fresh inventory publish to start working.  After consuming
     * internally we honour the filter flag: returning FALSE here
     * suppresses the downstream emit but the cache update above has
     * already happened, so the node still gets the data it needs. */
    if (strcmp (topic, PN_ZIGBEE_DEVICES_TOPIC) == 0)
    {
        rebuild_device_cache (self, message);
        if (self->filter_bridge_devices)
            return FALSE;
        return pn_mqtt_real_process_message (base, message);
    }

    /* bridge/definitions and bridge/info have no internal consumer
     * yet -- we still parse them (the base already did) but the
     * filter just suppresses the downstream emit.  Placed here, after
     * the bridge/devices branch, so its cache-rebuild side effect
     * isn't skipped. */
    if (self->filter_bridge_definitions &&
        strcmp (topic, PN_ZIGBEE_DEFINITIONS_TOPIC) == 0)
        return FALSE;

    if (self->filter_bridge_info &&
        strcmp (topic, PN_ZIGBEE_INFO_TOPIC) == 0)
        return FALSE;

    /* Skip any other bridge/... topic without lookup -- the
     * friendly-name index never contains a "bridge"-prefixed entry,
     * so the lookup would always miss, but the early bail keeps the
     * common case readable. */
    if (g_str_has_prefix (topic, PN_ZIGBEE_BRIDGE_PREFIX))
        return pn_mqtt_real_process_message (base, message);

    if (self->inject_device_info || self->inject_device_capabilities)
    {
        const gchar        *friendly_name;
        ZigbeeCachedDevice *cached;

        friendly_name = topic + strlen (PN_ZIGBEE_BASE_TOPIC "/");
        cached = g_hash_table_lookup (self->device_info, friendly_name);

        /* The cache key is the bare friendly_name, so a sub-topic
         * like `zigbee2mqtt/<name>/availability` naturally misses
         * (key would be `<name>/availability`); no extra suffix
         * filtering needed.  Non-state shapes (binary, plain
         * strings) similarly fall out via the HOLDS_OBJECT guard
         * inside inject_device_info_block. */
        if (cached != NULL)
            inject_device_info_block (message, cached,
                    self->inject_device_info,
                    self->inject_device_capabilities);
    }

    return pn_mqtt_real_process_message (base, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_zigbee_source_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnZigbeeSource *self = PN_ZIGBEE_SOURCE (object);

    switch (prop_id)
    {
    case PROP_FILTER_LOGGING:
        g_value_set_boolean (value,
                g_atomic_int_get (&self->filter_logging) != 0);
        break;
    case PROP_FILTER_BRIDGE_DEVICES:
        g_value_set_boolean (value, self->filter_bridge_devices);
        break;
    case PROP_FILTER_BRIDGE_DEFINITIONS:
        g_value_set_boolean (value, self->filter_bridge_definitions);
        break;
    case PROP_FILTER_BRIDGE_INFO:
        g_value_set_boolean (value, self->filter_bridge_info);
        break;
    case PROP_INJECT_DEVICE_INFO:
        g_value_set_boolean (value, self->inject_device_info);
        break;
    case PROP_INJECT_DEVICE_CAPABILITIES:
        g_value_set_boolean (value, self->inject_device_capabilities);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_zigbee_source_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnZigbeeSource *self = PN_ZIGBEE_SOURCE (object);

    switch (prop_id)
    {
    case PROP_FILTER_LOGGING:
    {
        gint want = g_value_get_boolean (value) ? 1 : 0;
        if (g_atomic_int_get (&self->filter_logging) != want)
        {
            g_atomic_int_set (&self->filter_logging, want);
            g_object_notify_by_pspec (object, props[PROP_FILTER_LOGGING]);
        }
        break;
    }
    case PROP_FILTER_BRIDGE_DEVICES:
    {
        gboolean want = g_value_get_boolean (value);
        if (self->filter_bridge_devices != want)
        {
            self->filter_bridge_devices = want;
            g_object_notify_by_pspec (object,
                    props[PROP_FILTER_BRIDGE_DEVICES]);
        }
        break;
    }
    case PROP_FILTER_BRIDGE_DEFINITIONS:
    {
        gboolean want = g_value_get_boolean (value);
        if (self->filter_bridge_definitions != want)
        {
            self->filter_bridge_definitions = want;
            g_object_notify_by_pspec (object,
                    props[PROP_FILTER_BRIDGE_DEFINITIONS]);
        }
        break;
    }
    case PROP_FILTER_BRIDGE_INFO:
    {
        gboolean want = g_value_get_boolean (value);
        if (self->filter_bridge_info != want)
        {
            self->filter_bridge_info = want;
            g_object_notify_by_pspec (object,
                    props[PROP_FILTER_BRIDGE_INFO]);
        }
        break;
    }
    case PROP_INJECT_DEVICE_INFO:
    {
        gboolean want = g_value_get_boolean (value);
        if (self->inject_device_info != want)
        {
            self->inject_device_info = want;
            g_object_notify_by_pspec (object, props[PROP_INJECT_DEVICE_INFO]);
        }
        break;
    }
    case PROP_INJECT_DEVICE_CAPABILITIES:
    {
        gboolean want = g_value_get_boolean (value);
        if (self->inject_device_capabilities != want)
        {
            self->inject_device_capabilities = want;
            g_object_notify_by_pspec (object,
                    props[PROP_INJECT_DEVICE_CAPABILITIES]);
        }
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_zigbee_source_finalize (GObject *object)
{
    PnZigbeeSource *self = PN_ZIGBEE_SOURCE (object);

    g_clear_pointer (&self->device_info, g_hash_table_destroy);

    G_OBJECT_CLASS (pn_zigbee_source_parent_class)->finalize (object);
}

static void
pn_zigbee_source_class_init (PnZigbeeSourceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);
    PnMqttClass  *mqtt_class   = PN_MQTT_CLASS (klass);

    object_class->get_property = pn_zigbee_source_get_property;
    object_class->set_property = pn_zigbee_source_set_property;
    object_class->finalize     = pn_zigbee_source_finalize;

    mqtt_class->accept_topic    = pn_zigbee_source_accept_topic;
    mqtt_class->process_message = pn_zigbee_source_process_message;

    node_class->palette_icon = PN_ZIGBEE_SOURCE_ICON;
    node_class->class_name   = "Zigbee Source";
    node_class->icon         = PN_ZIGBEE_SOURCE_ICON;
    node_class->color        = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category     = "Zigbee";

    props[PROP_FILTER_LOGGING] = g_param_spec_boolean (
            "filter-logging", "Filter logging",
            "When TRUE (default), publishes on "
            "zigbee2mqtt/bridge/logging -- Zigbee2MQTT's MQTT mirror "
            "of its own log lines, the channel the Z2M web UI's Logs "
            "tab consumes -- are dropped on the network thread before "
            "a PnMessage is built, so the rest of the flow only sees "
            "device traffic and structured bridge events.  Set FALSE "
            "to forward those log envelopes too (useful when piping "
            "Z2M logs into a Debug node or an external aggregator).  "
            "Has no effect on the other bridge/* topics (event, "
            "state, devices, groups, definitions, response/...): "
            "those carry structured data and always pass through.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FILTER_BRIDGE_DEVICES] = g_param_spec_boolean (
            "filter-bridge-devices", "Filter bridge/devices",
            "When TRUE (default), the retained "
            "zigbee2mqtt/bridge/devices inventory publish is consumed "
            "internally -- the device-info cache that drives the "
            "`device` block injection is still rebuilt from it -- but "
            "is not forwarded downstream, so the worksheet stays clean "
            "of the multi-KB inventory dump every time the device set "
            "changes or the node reconnects.  Set FALSE to forward "
            "the envelope too (useful when piping inventory into a "
            "Debug node or a third-party device-discovery consumer); "
            "the cache rebuild happens either way.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FILTER_BRIDGE_DEFINITIONS] = g_param_spec_boolean (
            "filter-bridge-definitions", "Filter bridge/definitions",
            "When TRUE (default), the retained "
            "zigbee2mqtt/bridge/definitions publish -- Z2M's full "
            "device-type definition catalogue, typically several MB "
            "-- is dropped on the main thread after parsing, so it "
            "does not flood downstream Debug nodes.  Filtered on "
            "process_message rather than accept_topic so a future "
            "internal consumer (e.g. richer category synthesis) can "
            "still see the parsed payload.  Set FALSE to forward it "
            "downstream.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FILTER_BRIDGE_INFO] = g_param_spec_boolean (
            "filter-bridge-info", "Filter bridge/info",
            "When TRUE (default), the retained "
            "zigbee2mqtt/bridge/info publish -- Z2M's bridge-status "
            "snapshot (version, coordinator, permit_join, network "
            "key, ...) -- is dropped on the main thread after "
            "parsing.  Filtered on process_message rather than "
            "accept_topic so a future internal consumer can still "
            "see the parsed payload.  Set FALSE to forward it "
            "downstream (useful for a bridge-status dashboard).",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_INJECT_DEVICE_INFO] = g_param_spec_boolean (
            "inject-device-info", "Inject device info",
            "When TRUE (default), the injected `device` block on "
            "each per-device state publish carries the Z2M "
            "include_device_information identity scalars: "
            "friendlyName, ieeeAddr, networkAddress, type, "
            "powerSource, manufacturerName, modelID, model, "
            "dateCode, softwareBuildID -- the camelCase shape Z2M "
            "emits when configured with include_device_information: "
            "true, so a Z2M-shape consumer works unchanged.  Fields "
            "not in bridge/devices (applicationVersion / "
            "stackVersion / zclVersion / hardwareVersion / "
            "manufacturerID, which Z2M sources from the in-process "
            "zigbee-herdsman device object) are omitted.  "
            "Independent of inject-device-capabilities: turn this "
            "off and capabilities on to get just the capability "
            "block without the duplicated identity scalars, or "
            "leave both on to splice the union.  The cache is "
            "still maintained while FALSE, so toggling back on "
            "takes effect on the next publish.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_INJECT_DEVICE_CAPABILITIES] = g_param_spec_boolean (
            "inject-device-capabilities", "Inject device capabilities",
            "When TRUE, the injected `device` block also carries "
            "the synthesised `category` one-word label (\"plug\", "
            "\"switch\", \"light\", \"lock\", \"cover\", \"fan\", "
            "\"climate\", \"remote\", \"sensor\", \"device\"), the "
            "human-readable `description` and `vendor`, a "
            "`supportsOta` boolean, and the structured `exposes` "
            "and `options` trees (Z2M's full capability + config "
            "descriptor with per-feature access bits, value ranges, "
            "enum value lists, units, labels).  Off by default "
            "because exposes / options can run several KB per "
            "device, making each state publish proportionally "
            "bigger -- enable when a downstream node actually needs "
            "to introspect device commands / configuration.  "
            "Independent of inject-device-info: this block only "
            "contains the capability / configuration / synthesised "
            "fields, so turning it on without info gives a focused "
            "capability descriptor (the publish's envelope topic "
            "already names the device, so duplicating friendlyName "
            "etc. is optional).",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_zigbee_source_init (PnZigbeeSource *self)
{
    self->filter_logging             = 1;
    self->filter_bridge_devices      = TRUE;
    self->filter_bridge_definitions  = TRUE;
    self->filter_bridge_info         = TRUE;
    self->inject_device_info         = TRUE;
    self->inject_device_capabilities = FALSE;
    self->device_info = g_hash_table_new_full (
            g_str_hash, g_str_equal,
            g_free, zigbee_cached_device_free);

    /* Default the inherited subscribe-filter to the Z2M topic root so
     * a freshly-dropped node starts pointed at the right tree.  The
     * worksheet loader applies stored properties AFTER instance init,
     * so a saved value in the file overrides this default the same
     * way it would any other property.  Setting via g_object_set
     * (rather than poking a private field) goes through the base
     * class's set_property, which is the supported path -- the
     * property is documented public on #PnMqtt. */
    g_object_set (self, "subscribe-topic", "zigbee2mqtt/#", NULL);
}

PnZigbeeSource *
pn_zigbee_source_new (void)
{
    return g_object_new (PN_TYPE_ZIGBEE_SOURCE, NULL);
}
