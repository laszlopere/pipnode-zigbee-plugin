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

#ifndef PN_ZIGBEE_NAME_REGISTRY_H
#define PN_ZIGBEE_NAME_REGISTRY_H

#include <glib-object.h>

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Background Zigbee friendly-name registry                           */
/*                                                                     */
/*  A process-global service that silently keeps a merged, deduplicated */
/*  set of Zigbee2MQTT device friendly names gathered from every        */
/*  configured mqtt-broker vault profile.  Unlike the "Zigbee Devices"  */
/*  dialog and the "Zigbee Source" node -- which learn names as part of */
/*  their own *visible* operation -- this collector runs entirely in the */
/*  background, so the friendly-name field of a Zigbee node's settings   */
/*  dialog can offer the names as a dropdown the moment the dialog opens.*/
/*                                                                     */
/*  Deliberately GTK-free: it depends only on pipnode-core (PnMqtt /     */
/*  PnVault / PnMessage) + json-glib, so its ingest logic is unit-       */
/*  testable network-free (see tests/test-zigbee-name-registry.c).  The  */
/*  GTK combo editor that consumes it lives in pn-zigbee-node-gui.c.     */
/* ------------------------------------------------------------------ */

/**
 * zb_name_registry_start:
 *
 * Begin (or, if already running, keep) the background collection.
 * Idempotent: safe to call more than once; only the first call opens the
 * broker connections.  Enumerates every #PN_PROFILE_TYPE_MQTT_BROKER
 * profile from the default vault and opens one hidden #PnMqtt source per
 * profile, narrowly subscribed to the retained `zigbee2mqtt/bridge/devices`
 * inventory topic.  Meant to be called once from pn_plugin_gui_init().
 */
void zb_name_registry_start (void);

/**
 * zb_name_registry_ingest_devices:
 * @payload: a `zigbee2mqtt/bridge/devices` message payload -- expected to be
 *           a #JsonNode holding an array of device objects
 *
 * Merge every device in @payload into the known set, recording each device's
 * `friendly_name` and a human-readable type -- the nested
 * `definition.description` ("Water leak detector", "Security remote control",
 * ...), falling back to the top-level `type` ("Coordinator") when there is no
 * definition.  Also records a one-word device category (see
 * zb_name_registry_lookup_category()).  Emits "changed" (see
 * zb_name_registry_get_object()) if a name was added or first gained a
 * type/category.  A @payload that is %NULL or not an array
 * is ignored, so a malformed publish never blanks a good set.  Called from the
 * hidden sources' message handler; exposed so the ingest logic can be
 * exercised network-free.
 */
void zb_name_registry_ingest_devices (JsonNode *payload);

/**
 * zb_name_registry_get_names:
 *
 * Snapshot the currently-known friendly names.
 *
 * Returns: (transfer full) (element-type utf8): a freshly-allocated,
 *   alphabetically-sorted #GPtrArray of newly-allocated name strings
 *   (never %NULL; may be empty).  Free with g_ptr_array_unref().
 */
GPtrArray *zb_name_registry_get_names (void);

/**
 * zb_name_registry_get_type:
 * @name: a friendly name (typically one returned by zb_name_registry_get_names())
 *
 * The human-readable device type recorded for @name -- the second, dimmer
 * line of the combo dropdown.
 *
 * Returns: (transfer full) (nullable): a newly-allocated type string, or %NULL
 *   when @name is unknown or has no type.  Free with g_free().
 */
gchar *zb_name_registry_lookup_type (const gchar *name);

/**
 * zb_name_registry_lookup_category:
 * @name: a friendly name (typically one returned by zb_name_registry_get_names())
 *
 * The one-word device class recorded for @name -- "light", "plug", "switch",
 * "leak", "sensor", "remote", "lock", "cover", "fan", "climate",
 * "coordinator", or "device".  The combo maps it to a type glyph shown to the
 * left of the two text lines.
 *
 * Returns: (transfer full) (nullable): a newly-allocated category string, or
 *   %NULL when @name is unknown.  Free with g_free().
 */
gchar *zb_name_registry_lookup_category (const gchar *name);

/**
 * zb_name_registry_get_object:
 *
 * The registry's backing #GObject, which emits a parameterless "changed"
 * signal whenever the known-name set changes.  A dropdown built before the
 * first `bridge/devices` payload landed (or while a device is being paired)
 * connects to it -- typically via g_signal_connect_object() so the handler
 * dies with the widget -- to repopulate live.
 *
 * Returns: (transfer none): the singleton, valid for the process lifetime.
 */
GObject *zb_name_registry_get_object (void);

G_END_DECLS

#endif /* PN_ZIGBEE_NAME_REGISTRY_H */
