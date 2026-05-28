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
/*  The remaining `bridge/...` topics (event, state, devices, groups,  */
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
