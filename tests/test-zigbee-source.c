/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * Unit tests for PnZigbeeSource: the Z2M-aware MQTT source.  It subclasses
 * PnMqtt and adds two hooks on the inbound publish stream -- an early
 * accept_topic filter (drops bridge/logging) and a late process_message
 * hook (rebuilds the device-info cache from bridge/devices, suppresses the
 * bulky bridge envelopes, and grafts a `device` block onto per-device
 * state publishes).
 *
 * No broker, no network, no stub library.  PnMqtt's connect is debounced
 * onto a main-loop idle (schedule_restart -> g_idle_add) and bails on an
 * empty URL regardless; this suite never pumps a GLib main loop, so that
 * idle never fires and the queued source is cancelled in dispose when the
 * node is unref'd -- libmosquitto is never entered.  The two hooks under
 * test are public PnMqttClass vfuncs, so we drive them directly through
 * PN_MQTT_GET_CLASS() exactly as the base would on the network thread,
 * but synchronously and on no thread but our own.
 */

#include "pn-test.h"
#include "pn-zigbee-source.h"

#include <pn-mqtt.h>
#include <json-glib/json-glib.h>

/* fa-rss U+F09E -- the node's healthy glyph (shared with the base PnMqtt). */
#define SOURCE_ICON "\xef\x82\x9e"

#define BASE_TOPIC        "zigbee2mqtt"
#define LOGGING_TOPIC     "zigbee2mqtt/bridge/logging"
#define DEVICES_TOPIC     "zigbee2mqtt/bridge/devices"
#define DEFINITIONS_TOPIC "zigbee2mqtt/bridge/definitions"
#define INFO_TOPIC        "zigbee2mqtt/bridge/info"

/* A realistic bridge/devices inventory exercising every category branch of
 * synthesize_category(): a mains light, a mains switch (-> plug), a battery
 * switch (-> switch), a read-only sensor, an action remote, and a
 * definition-less coordinator (-> device).  The lamp also carries the full
 * identity + capability field set so the injection tests can assert the
 * mapped camelCase shape. */
static const char *DEVICES_JSON =
    "["
      "{"
        "\"friendly_name\":\"lamp\","
        "\"ieee_address\":\"0x00124b00\","
        "\"network_address\":4660,"
        "\"type\":\"Router\","
        "\"power_source\":\"Mains (single phase)\","
        "\"manufacturer\":\"Acme\","
        "\"model_id\":\"TS0505B\","
        "\"definition\":{"
          "\"model\":\"LAMP-1\",\"vendor\":\"Acme\","
          "\"description\":\"Smart bulb\",\"supports_ota\":true,"
          "\"exposes\":[{\"type\":\"light\","
            "\"features\":[{\"property\":\"state\",\"access\":7}]}],"
          "\"options\":[]"
        "}"
      "},"
      "{\"friendly_name\":\"plug\",\"power_source\":\"Mains (single phase)\","
        "\"definition\":{\"exposes\":[{\"type\":\"switch\","
          "\"features\":[{\"property\":\"state\",\"access\":7}]}]}},"
      "{\"friendly_name\":\"button\",\"power_source\":\"Battery\","
        "\"definition\":{\"exposes\":[{\"type\":\"switch\","
          "\"features\":[{\"property\":\"state\",\"access\":7}]}]}},"
      "{\"friendly_name\":\"thermo\","
        "\"definition\":{\"exposes\":[{\"property\":\"temperature\","
          "\"access\":1}]}},"
      "{\"friendly_name\":\"scene\","
        "\"definition\":{\"exposes\":[{\"property\":\"action\","
          "\"access\":1}]}},"
      "{\"friendly_name\":\"coord\"}"
    "]";

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Deep-copy a freshly-parsed JSON document into an owned node. */
static JsonNode *
parse_json (const char *json)
{
    JsonParser *parser = json_parser_new ();
    JsonNode   *root;

    g_assert (json_parser_load_from_data (parser, json, -1, NULL));
    root = json_node_copy (json_parser_get_root (parser));
    g_object_unref (parser);
    return root;
}

/* Build a publish: envelope topic + a `payload` member parsed from JSON,
 * mirroring what pn_mqtt_build_message() hands process_message(). */
static PnMessage *
publish (const char *topic, const char *payload_json)
{
    PnMessage *msg = pn_message_new (NULL, topic);
    pn_message_set_member (msg, "payload", parse_json (payload_json));
    return msg;
}

/* A routine per-device state publish (an object payload, as Z2M sends). */
static PnMessage *
state_publish (const char *topic)
{
    return publish (topic, "{\"state\":\"ON\",\"linkquality\":120}");
}

/* Drive the subclass's accept_topic vfunc directly (network-thread hook). */
static gboolean
run_accept (PnZigbeeSource *src, const char *topic)
{
    return PN_MQTT_GET_CLASS (src)->accept_topic (PN_MQTT (src), topic);
}

/* Drive the subclass's process_message vfunc directly (main-thread hook). */
static gboolean
run_process (PnZigbeeSource *src, PnMessage *msg)
{
    return PN_MQTT_GET_CLASS (src)->process_message (PN_MQTT (src), msg);
}

/* The `device` block process_message grafted onto data.payload, or NULL
 * when none was injected / the payload is not an object. */
static JsonObject *
injected_device (PnMessage *msg)
{
    JsonNode   *payload_node = pn_message_get_member (msg, "payload");
    JsonObject *payload;

    if (payload_node == NULL || !JSON_NODE_HOLDS_OBJECT (payload_node))
        return NULL;
    payload = json_node_get_object (payload_node);
    if (payload == NULL || !json_object_has_member (payload, "device"))
        return NULL;
    return json_object_get_object_member (payload, "device");
}

/* Feed the standard inventory and confirm it was consumed (filtered). */
static void
prime_cache (PnZigbeeSource *src)
{
    PnMessage *devs = publish (DEVICES_TOPIC, DEVICES_JSON);
    CHECK_FALSE (run_process (src, devs));   /* filtered by default */
    g_object_unref (devs);
}

/* Inject and return the synthesised category for a primed friendly-name.
 * The category string is borrowed from the message, which is freed before
 * we return, so copy it into a static scratch buffer the caller can safely
 * g_strcmp0 against (one call in flight at a time -- fine for this suite). */
static const char *
category_of (PnZigbeeSource *src, const char *friendly_name)
{
    static char buf[32];
    char       *topic = g_strconcat (BASE_TOPIC "/", friendly_name, NULL);
    PnMessage  *st    = state_publish (topic);
    JsonObject *dev;

    run_process (src, st);
    dev = injected_device (st);
    buf[0] = '\0';
    if (dev != NULL && json_object_has_member (dev, "category"))
        g_strlcpy (buf, json_object_get_string_member (dev, "category"),
                   sizeof buf);

    g_object_unref (st);
    g_free (topic);
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

static void
test_defaults (void)
{
    PnNode         *node = PN_NODE (pn_zigbee_source_new ());
    PnZigbeeSource *src  = (PnZigbeeSource *) node;
    gboolean flog, fdev, fdef, finfo, iinfo, icaps;
    char    *sub = NULL;

    /* Class metadata. */
    CHECK_STR_EQ (pn_node_get_class_name (node), "Zigbee Source");
    CHECK_STR_EQ (pn_node_get_category (node),   "Zigbee");
    CHECK_STR_EQ (pn_node_get_icon (node),       SOURCE_ICON);

    /* A source: output only, no input. */
    CHECK (pn_node_get_has_output (node));
    CHECK_FALSE (pn_node_get_has_input (node));

    /* Property defaults: filters on, info on, capabilities off; subscribe
     * filter seeded to the Z2M root. */
    g_object_get (src,
                  "filter-logging",             &flog,
                  "filter-bridge-devices",      &fdev,
                  "filter-bridge-definitions",  &fdef,
                  "filter-bridge-info",         &finfo,
                  "inject-device-info",         &iinfo,
                  "inject-device-capabilities", &icaps,
                  "subscribe-topic",            &sub,
                  NULL);
    CHECK (flog);
    CHECK (fdev);
    CHECK (fdef);
    CHECK (finfo);
    CHECK (iinfo);
    CHECK_FALSE (icaps);
    CHECK_STR_EQ (sub, "zigbee2mqtt/#");

    g_free (sub);
    g_object_unref (node);
}

static void
test_accept_topic_filters_logging (void)
{
    PnZigbeeSource *src = pn_zigbee_source_new ();

    /* filter-logging on (default): only bridge/logging is dropped. */
    CHECK_FALSE (run_accept (src, LOGGING_TOPIC));
    CHECK (run_accept (src, "zigbee2mqtt/lamp"));
    CHECK (run_accept (src, DEVICES_TOPIC));

    /* Turn it off: the log mirror is forwarded too. */
    g_object_set (src, "filter-logging", FALSE, NULL);
    CHECK (run_accept (src, LOGGING_TOPIC));

    g_object_unref (src);
}

static void
test_bridge_devices_filtered_and_cached (void)
{
    PnZigbeeSource *src = pn_zigbee_source_new ();
    PnMessage      *st;
    JsonObject     *dev;

    /* bridge/devices is consumed (cache rebuilt) and, by default, dropped
     * from the downstream stream. */
    prime_cache (src);

    /* A per-device state publish now gets the identity block grafted in.
     * inject-device-info is on by default, capabilities off, so we expect
     * the Z2M camelCase identity scalars and NONE of the capability keys. */
    st = state_publish ("zigbee2mqtt/lamp");
    CHECK (run_process (src, st));   /* state publishes pass downstream */
    dev = injected_device (st);
    CHECK_NOT_NULL (dev);
    if (dev != NULL)
    {
        CHECK_STR_EQ (json_object_get_string_member (dev, "friendlyName"),
                      "lamp");
        CHECK_STR_EQ (json_object_get_string_member (dev, "ieeeAddr"),
                      "0x00124b00");
        CHECK_INT_EQ (json_object_get_int_member (dev, "networkAddress"),
                      4660);
        CHECK_STR_EQ (json_object_get_string_member (dev, "type"), "Router");
        CHECK_STR_EQ (json_object_get_string_member (dev, "manufacturerName"),
                      "Acme");
        CHECK_STR_EQ (json_object_get_string_member (dev, "modelID"),
                      "TS0505B");
        CHECK_STR_EQ (json_object_get_string_member (dev, "model"), "LAMP-1");
        /* Capability keys must be absent while inject-capabilities is off. */
        CHECK_FALSE (json_object_has_member (dev, "category"));
        CHECK_FALSE (json_object_has_member (dev, "exposes"));
        CHECK_FALSE (json_object_has_member (dev, "description"));
    }

    g_object_unref (st);
    g_object_unref (src);
}

static void
test_inject_capabilities (void)
{
    PnZigbeeSource *src = pn_zigbee_source_new ();
    PnMessage      *st;
    JsonObject     *dev;

    g_object_set (src, "inject-device-capabilities", TRUE, NULL);
    prime_cache (src);

    st  = state_publish ("zigbee2mqtt/lamp");
    run_process (src, st);
    dev = injected_device (st);
    CHECK_NOT_NULL (dev);
    if (dev != NULL)
    {
        /* Identity block still present (both flags on -> union). */
        CHECK_STR_EQ (json_object_get_string_member (dev, "friendlyName"),
                      "lamp");
        /* Capability block now contributes its keys. */
        CHECK_STR_EQ (json_object_get_string_member (dev, "category"),
                      "light");
        CHECK_STR_EQ (json_object_get_string_member (dev, "description"),
                      "Smart bulb");
        CHECK_STR_EQ (json_object_get_string_member (dev, "vendor"), "Acme");
        CHECK (json_object_get_boolean_member (dev, "supportsOta"));
        CHECK (json_object_has_member (dev, "exposes"));
        CHECK (JSON_NODE_HOLDS_ARRAY (
                   json_object_get_member (dev, "exposes")));
    }

    g_object_unref (st);
    g_object_unref (src);
}

static void
test_inject_disabled_adds_no_block (void)
{
    PnZigbeeSource *src = pn_zigbee_source_new ();
    PnMessage      *st;

    g_object_set (src,
                  "inject-device-info",         FALSE,
                  "inject-device-capabilities", FALSE,
                  NULL);
    prime_cache (src);

    st = state_publish ("zigbee2mqtt/lamp");
    CHECK (run_process (src, st));
    CHECK_NULL (injected_device (st));   /* no `device` member at all */

    g_object_unref (st);
    g_object_unref (src);
}

static void
test_category_synthesis (void)
{
    PnZigbeeSource *src = pn_zigbee_source_new ();

    g_object_set (src, "inject-device-capabilities", TRUE, NULL);
    prime_cache (src);

    CHECK_STR_EQ (category_of (src, "lamp"),   "light");
    CHECK_STR_EQ (category_of (src, "plug"),   "plug");   /* switch + mains */
    CHECK_STR_EQ (category_of (src, "button"), "switch"); /* switch + battery */
    CHECK_STR_EQ (category_of (src, "thermo"), "sensor"); /* all read-only */
    CHECK_STR_EQ (category_of (src, "scene"),  "remote"); /* property=action */
    CHECK_STR_EQ (category_of (src, "coord"),  "device"); /* no definition */

    g_object_unref (src);
}

static void
test_malformed_devices_keeps_cache_and_warns (void)
{
    PnZigbeeSource *src = pn_zigbee_source_new ();
    PnMessage      *bad, *st;

    /* Prime with a good inventory, prove the cache works. */
    prime_cache (src);
    st = state_publish ("zigbee2mqtt/lamp");
    run_process (src, st);
    CHECK_NOT_NULL (injected_device (st));
    g_object_unref (st);

    /* A non-array bridge/devices publish must be rejected without wiping
     * the cache, and surface a WARNING (PLUGINS section 12, channel 3). */
    bad = publish (DEVICES_TOPIC, "{\"oops\":true}");
    CHECK_FALSE (run_process (src, bad));   /* still filtered */
    g_object_unref (bad);
    CHECK_INT_EQ (t_log_count (PN_NODE (src), PN_LOG_LEVEL_WARNING), 1);
    CHECK (t_log_contains (PN_NODE (src), PN_LOG_LEVEL_WARNING,
                           "not a JSON array"));

    /* The previous inventory survived: lamp still decorates. */
    st = state_publish ("zigbee2mqtt/lamp");
    run_process (src, st);
    CHECK_NOT_NULL (injected_device (st));
    g_object_unref (st);

    g_object_unref (src);
}

static void
test_unusable_entries_aggregated_warning (void)
{
    PnZigbeeSource *src = pn_zigbee_source_new ();
    PnMessage      *devs;

    /* One usable entry plus two that yield no friendly_name (an object
     * missing the key, and a bare string): exactly one aggregated WARNING
     * naming the skipped count, not per-element spam. */
    devs = publish (DEVICES_TOPIC,
                    "[{\"friendly_name\":\"ok\"},{\"no_name\":true},\"junk\"]");
    run_process (src, devs);
    g_object_unref (devs);

    CHECK_INT_EQ (t_log_count (PN_NODE (src), PN_LOG_LEVEL_WARNING), 1);
    CHECK (t_log_contains (PN_NODE (src), PN_LOG_LEVEL_WARNING, "skipped 2"));

    g_object_unref (src);
}

static void
test_bridge_filters_toggle (void)
{
    PnZigbeeSource *src = pn_zigbee_source_new ();
    PnMessage      *m;

    /* Defaults: definitions + info are suppressed downstream. */
    m = publish (DEFINITIONS_TOPIC, "{}");
    CHECK_FALSE (run_process (src, m));
    g_object_unref (m);

    m = publish (INFO_TOPIC, "{}");
    CHECK_FALSE (run_process (src, m));
    g_object_unref (m);

    /* Turn the filters off: the envelopes now pass downstream. */
    g_object_set (src,
                  "filter-bridge-definitions", FALSE,
                  "filter-bridge-info",        FALSE,
                  NULL);

    m = publish (DEFINITIONS_TOPIC, "{}");
    CHECK (run_process (src, m));
    g_object_unref (m);

    m = publish (INFO_TOPIC, "{}");
    CHECK (run_process (src, m));
    g_object_unref (m);

    /* bridge/devices forwarded when unfiltered -- and the cache still
     * rebuilds, so injection keeps working either way. */
    g_object_set (src, "filter-bridge-devices", FALSE, NULL);
    m = publish (DEVICES_TOPIC, DEVICES_JSON);
    CHECK (run_process (src, m));
    g_object_unref (m);

    m = state_publish ("zigbee2mqtt/lamp");
    run_process (src, m);
    CHECK_NOT_NULL (injected_device (m));
    g_object_unref (m);

    g_object_unref (src);
}

static void
test_non_device_topics_pass_through (void)
{
    PnZigbeeSource *src = pn_zigbee_source_new ();
    PnMessage      *m;

    prime_cache (src);

    /* A non-Zigbee topic is forwarded untouched (no `device` graft). */
    m = state_publish ("home/livingroom/temperature");
    CHECK (run_process (src, m));
    CHECK_NULL (injected_device (m));
    g_object_unref (m);

    /* A sub-topic (availability) misses the bare-friendly-name cache key. */
    m = state_publish ("zigbee2mqtt/lamp/availability");
    CHECK (run_process (src, m));
    CHECK_NULL (injected_device (m));
    g_object_unref (m);

    /* A non-object payload (Z2M's bare-string availability) is left alone
     * by the injector even on an exact friendly-name match. */
    m = publish ("zigbee2mqtt/lamp", "\"online\"");
    CHECK (run_process (src, m));
    CHECK_NULL (injected_device (m));
    g_object_unref (m);

    g_object_unref (src);
}

int
main (int argc, char **argv)
{
    t_init (&argc, &argv, "pn-zigbee-source");
    t_add ("defaults",                     test_defaults);
    t_add ("accept_topic_filters_logging", test_accept_topic_filters_logging);
    t_add ("bridge_devices_filtered_cached", test_bridge_devices_filtered_and_cached);
    t_add ("inject_capabilities",          test_inject_capabilities);
    t_add ("inject_disabled_no_block",     test_inject_disabled_adds_no_block);
    t_add ("category_synthesis",           test_category_synthesis);
    t_add ("malformed_devices_keeps_cache", test_malformed_devices_keeps_cache_and_warns);
    t_add ("unusable_entries_warning",     test_unusable_entries_aggregated_warning);
    t_add ("bridge_filters_toggle",        test_bridge_filters_toggle);
    t_add ("non_device_topics_passthrough", test_non_device_topics_pass_through);
    return t_run ();
}
