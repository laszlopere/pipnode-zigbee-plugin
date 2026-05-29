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

/* ------------------------------------------------------------------ */
/*  Pipnode Zigbee plugin                                              */
/*                                                                     */
/*  Out-of-tree plugin shipping nodes that interact with Zigbee        */
/*  devices via a Zigbee2MQTT instance over MQTT.  The Zigbee dongle   */
/*  itself (e.g. AVATTO GW70-MQTT, a CC2652P + CP2102N stick) is a     */
/*  USB-serial Zigbee radio, not an MQTT endpoint; the Z2M daemon      */
/*  drives the radio and translates each Zigbee message into JSON      */
/*  published on `zigbee2mqtt/<friendly_name>` topics.  Nodes here     */
/*  therefore speak MQTT to a broker -- typically the same one Z2M     */
/*  publishes to -- and follow the documented Z2M topic / payload      */
/*  contract.                                                          */
/*                                                                     */
/*  Currently registered nodes:                                        */
/*                                                                     */
/*    * PnZigbeeSource -- PnMqtt subclass, defaults to subscribing     */
/*      zigbee2mqtt/# and drops the noisy bridge/logging channel.      */
/*    * PnZigbeeSwitch -- bidirectional on/off slider for a single     */
/*      Z2M endpoint (zigbee2mqtt/<friendly_name> in, .../set out).    */
/*    * PnZigbeeRelayStatus -- receive-only decoder, the factored      */
/*      read-half of the switch (state publish in, value message out). */
/*    * PnZigbeeRelayCommand -- send-only command builder, the         */
/*      factored write-half (value message in, .../set command out).   */
/*    * PnZigbeeRemote -- filters remote button-press events (payload  */
/*      `action`) and reshapes them into the canonical message shape.  */
/*    * PnZnpPing -- low-level liveness probe that talks ZNP over USB  */
/*      serial directly to the coordinator dongle.                     */
/*                                                                     */
/*  Further nodes (ZigbeeEvent, ZigbeePair, ZigbeeBridgeStatus, ...)   */
/*  will be added alongside their pn-zigbee-<node>.c / .h source and   */
/*  a help/<TypeName>.html page.                                       */
/*                                                                     */
/*  TODO: add a PnZigbeeSink -- the publish-side companion to          */
/*  PnZigbeeSource -- once the host's PnMqttSink becomes derivable.    */
/*  PnZigbeeSource works by subclassing the derivable PnMqtt base;     */
/*  the symmetric sink would subclass PnMqttSink, but that type is     */
/*  currently declared with G_DECLARE_FINAL_TYPE (non-derivable) in    */
/*  ../pipnode/lib/pn-mqtt-sink.h, so it cannot be specialised here.   */
/*  When it is switched to G_DECLARE_DERIVABLE_TYPE, wire up a         */
/*  Zigbee2MQTT-flavoured sink (default zigbee2mqtt/<name>/set topic,  */
/*  Z2M-shaped payload) the same way PnZigbeeSource specialises the    */
/*  source.                                                            */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

#include <pn-node-factory.h>
#include <pn-plugin.h>

#include "pn-zigbee-relay-command.h"
#include "pn-zigbee-relay-status.h"
#include "pn-zigbee-remote.h"
#include "pn-zigbee-source.h"
#include "pn-zigbee-switch.h"
#include "pn-znp-ping.h"

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-zigbee",
        .version     = "0.1.0",
        .description = "Zigbee device control / event nodes driven through a "
                       "Zigbee2MQTT bridge over MQTT, plus low-level ZNP "
                       "diagnostic nodes that talk to the coordinator dongle "
                       "directly over USB serial.",
    };

    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_SOURCE);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_SWITCH);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_RELAY_STATUS);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_RELAY_COMMAND);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_REMOTE);
    pn_node_factory_register (factory, PN_TYPE_ZNP_PING);

    return &info;
}
