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

#ifndef PN_ZIGBEE_SINK_H
#define PN_ZIGBEE_SINK_H

#include <pn-mqtt-sink.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZigbeeSink                                                       */
/*                                                                     */
/*  Zigbee2MQTT-flavoured MQTT sink -- the publish-side companion to    */
/*  #PnZigbeeSource, the same way #PnMqttSink is the input-only         */
/*  counterpart of #PnMqtt.  Subclass of #PnMqttSink; the connect /     */
/*  publish / offline-queue / reconnect / visual-state machinery stays  */
/*  in the base.                                                        */
/*                                                                     */
/*  For now this class only specialises the node's *appearance* so it   */
/*  sits with the rest of the Zigbee palette (magenta body, "Zigbee     */
/*  Sink" label, Zigbee category) -- it does not yet override either of  */
/*  #PnMqttSink's extension points, so it publishes exactly what the     */
/*  base does (each inbound message to its envelope topic, through the   */
/*  configured broker).  A Z2M-flavoured topic / payload default        */
/*  (zigbee2mqtt/<name>/set, Z2M-shaped payload) can be layered on       */
/*  later by overriding #PnMqttSinkClass::process_message, mirroring     */
/*  how #PnZigbeeSource specialises #PnMqtt.                            */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ZIGBEE_SINK (pn_zigbee_sink_get_type ())

G_DECLARE_FINAL_TYPE (PnZigbeeSink, pn_zigbee_sink,
                      PN, ZIGBEE_SINK, PnMqttSink)

PnZigbeeSink *pn_zigbee_sink_new (void);

G_END_DECLS

#endif /* PN_ZIGBEE_SINK_H */
