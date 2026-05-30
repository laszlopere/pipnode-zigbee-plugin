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

#ifndef PN_ZIGBEE_PERMIT_JOIN_H
#define PN_ZIGBEE_PERMIT_JOIN_H

#include <pn-node.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZigbeePermitJoin                                                 */
/*                                                                     */
/*  Send-only command builder that opens the Zigbee2MQTT pairing       */
/*  (permit-join) window for a preset interval.  Where                 */
/*  #PnZigbeeRelayCommand is the factored write-half for a single      */
/*  device's on/off `set`, this node is the write-half for the bridge  */
/*  request that lets new devices join the network.                    */
/*                                                                     */
/*  It treats any inbound message purely as a trigger (an Inject, a    */
/*  button, a schedule, ...): on every message it rewrites the         */
/*  envelope so a downstream MQTT Sink publishes straight to the Z2M   */
/*  bridge request topic:                                              */
/*                                                                     */
/*    topic   = zigbee2mqtt/bridge/request/permit_join                 */
/*    payload = { "value": true, "time": <seconds> }                   */
/*                                                                     */
/*  The window length is configured in MINUTES on the node and         */
/*  converted to the SECONDS that Z2M expects (minutes * 60).  The     */
/*  modern `{"value":true,"time":N}` form is used; older Z2M also      */
/*  accepts the bare `{"time":N}` shape this is a superset of.  The    */
/*  payload is set as a structured JsonNode object (not a              */
/*  pre-stringified string) so #PnMqttSink serialises it on the wire   */
/*  and a Debug node sees the structured form -- exactly the contract  */
/*  #PnZigbeeRelayCommand uses.                                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ZIGBEE_PERMIT_JOIN (pn_zigbee_permit_join_get_type ())

G_DECLARE_FINAL_TYPE (PnZigbeePermitJoin, pn_zigbee_permit_join,
                      PN, ZIGBEE_PERMIT_JOIN, PnNode)

PnZigbeePermitJoin *pn_zigbee_permit_join_new (void);

G_END_DECLS

#endif /* PN_ZIGBEE_PERMIT_JOIN_H */
