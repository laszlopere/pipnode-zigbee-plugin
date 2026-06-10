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

#ifndef PN_ZIGBEE_SOURCE_H
#define PN_ZIGBEE_SOURCE_H

#include <pn-mqtt.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZigbeeSource                                                     */
/*                                                                     */
/*  Zigbee2MQTT-flavoured MQTT source.  Subclass of #PnMqtt; the       */
/*  connect / subscribe / reconnect / visual-state machinery stays    */
/*  in the base, this class only specialises:                          */
/*                                                                     */
/*    * The default subscribe filter (`zigbee2mqtt/#`), so a fresh     */
/*      node points at the Z2M topic tree without configuration.      */
/*                                                                     */
/*    * An #PnMqttClass::accept_topic override that optionally drops   */
/*      `zigbee2mqtt/bridge/logging` on the network thread before a    */
/*      #PnMessage is even built -- the topic Z2M uses to mirror its   */
/*      own log lines to MQTT, useful for the web UI's log pane but    */
/*      pure noise for a flow that only wants device traffic.  The    */
/*      filter is gated by the boolean #PnZigbeeSource:filter-logging  */
/*      property, default %TRUE.                                       */
/*                                                                     */
/*    * An #PnMqttClass::process_message override that watches         */
/*      `zigbee2mqtt/bridge/devices` (Z2M's retained device-inventory  */
/*      publish, delivered automatically on each fresh subscribe)     */
/*      and caches a per-friendly-name device-info record in memory.   */
/*      Subsequent per-device state publishes get a `device` object   */
/*      grafted onto `data.payload` -- two opt-ins:                   */
/*                                                                     */
/*        - #PnZigbeeSource:inject-device-info (default %TRUE) emits   */
/*          the small Z2M `include_device_information`-shaped block   */
/*          (friendlyName / ieeeAddr / type / manufacturerName / ...). */
/*                                                                     */
/*        - #PnZigbeeSource:inject-device-capabilities (default       */
/*          %FALSE) extends the block with a synthesised category, a  */
/*          human-readable description / vendor / supportsOta, and    */
/*          the structured `exposes` + `options` trees from Z2M's     */
/*          definition.  Off by default because those trees can run   */
/*          to several KB per device.                                  */
/*                                                                     */
/*      The decoration is produced client-side from the cache, so no  */
/*      Z2M configuration change is needed and Z2M does not pay the    */
/*      per-publish bandwidth cost it would if it had been told to    */
/*      emit `include_device_information` upstream.                    */
/*                                                                     */
/*  The remaining `bridge/...` topics (event, state, groups,           */
/*  definitions, response/...) carry useful structured data and are   */
/*  always passed through; users who want a stricter cut can narrow   */
/*  the subscribe filter directly.                                    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ZIGBEE_SOURCE (pn_zigbee_source_get_type ())

G_DECLARE_FINAL_TYPE (PnZigbeeSource, pn_zigbee_source,
                      PN, ZIGBEE_SOURCE, PnMqtt)

PnZigbeeSource *pn_zigbee_source_new (void);

G_END_DECLS

#endif /* PN_ZIGBEE_SOURCE_H */
