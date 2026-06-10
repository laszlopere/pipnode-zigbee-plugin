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
/*    * PnZigbeeSink -- PnMqttSink subclass, the publish-side          */
/*      companion to PnZigbeeSource; currently a Zigbee-flavoured skin  */
/*      over the base sink (same publish behaviour, Zigbee palette).   */
/*    * PnZigbeeSwitch -- bidirectional on/off slider for a single     */
/*      Z2M endpoint (zigbee2mqtt/<friendly_name> in, .../set out).    */
/*    * PnZigbeeRelayStatus -- receive-only decoder, the factored      */
/*      read-half of the switch (state publish in, value message out). */
/*    * PnZigbeeRelayCommand -- send-only command builder, the         */
/*      factored write-half (value message in, .../set command out).   */
/*    * PnZigbeeRemote -- filters remote button-press events (payload  */
/*      `action`) and reshapes them into the canonical message shape.  */
/*    * PnZigbeeButton -- button-flavoured sibling of PnZigbeeRemote    */
/*      for a single wireless button (e.g. SONOFF SNZB-01P).           */
/*    * PnZigbeeWaterLeak -- edge-filters a leak sensor's boolean       */
/*      `water_leak` (begin / end / both) and reshapes survivors.      */
/*                                                                     */
/*  Further nodes (ZigbeeEvent, ZigbeePair, ZigbeeBridgeStatus, ...)   */
/*  will be added alongside their pn-zigbee-<node>.c / .h source and   */
/*  a help/<TypeName>.html page.                                       */
/*                                                                     */
/*  PnZigbeeSink now subclasses the host's (newly derivable) PnMqttSink */
/*  for its Zigbee identity only; a Z2M-flavoured topic / payload       */
/*  default (zigbee2mqtt/<name>/set, Z2M-shaped payload) can be layered */
/*  on later by overriding PnMqttSinkClass::process_message, the same   */
/*  way PnZigbeeSource specialises PnMqtt.                              */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

#include <pn-node-factory.h>
#include <pn-plugin.h>

#include "pn-zigbee-button.h"
#include "pn-zigbee-permit-join.h"
#include "pn-zigbee-relay-command.h"
#include "pn-zigbee-relay-status.h"
#include "pn-zigbee-remote.h"
#include "pn-zigbee-sink.h"
#include "pn-zigbee-source.h"
#include "pn-zigbee-switch.h"
#include "pn-zigbee-water-leak.h"

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-zigbee",
        .version     = "0.1.0",
        .description = "Zigbee device control / event nodes driven through a "
                       "Zigbee2MQTT bridge over MQTT.",
    };

    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_SOURCE);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_SINK);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_SWITCH);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_RELAY_STATUS);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_RELAY_COMMAND);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_REMOTE);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_BUTTON);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_WATER_LEAK);
    pn_node_factory_register (factory, PN_TYPE_ZIGBEE_PERMIT_JOIN);

    return &info;
}
