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
