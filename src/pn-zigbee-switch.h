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

#ifndef PN_ZIGBEE_SWITCH_H
#define PN_ZIGBEE_SWITCH_H

#include <pn-switch.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZigbeeSwitch                                                     */
/*                                                                     */
/*  Bidirectional device node for a single Zigbee on/off endpoint      */
/*  exposed by a Zigbee2MQTT bridge.  Same role as #PnTasmotaSwitch    */
/*  but for the Z2M side: the canvas-side slide switch, the inbound    */
/*  state filter and the outbound `set` command builder collapse       */
/*  into one node so the canonical "show the relay state, let the      */
/*  user flip it" worksheet wires up as                                */
/*                                                                     */
/*    MQTT Source -> Zigbee Switch -> MQTT Sink                        */
/*                                                                     */
/*  Subclass of #PnSwitch so the slider widget, hit-testing and the    */
/*  worksheet's slider-click routing all work for free via             */
/*  polymorphism (`PN_IS_SWITCH` returns TRUE on this class too).      */
/*                                                                     */
/*  Direction split:                                                   */
/*                                                                     */
/*    * Inbound: a Z2M state publish on                                */
/*      `zigbee2mqtt/<friendly_name>` whose JSON payload object        */
/*      carries `"state": "ON"` (or "OFF") syncs the latch             */
/*      *silently* -- no downstream emit, since the publish is an      */
/*      authoritative state observation rather than a transit          */
/*      message that should fan out.                                   */
/*                                                                     */
/*    * User click on the slider emits a fresh Z2M command             */
/*      message: envelope topic                                        */
/*      `zigbee2mqtt/<friendly_name>/set`, JSON `data.payload`         */
/*      object `{ "state": "ON" }` / `{ "state": "OFF" }`, plus the    */
/*      standard `data.value` / `data.success` / `data.device` /       */
/*      `data.output` mirror so a downstream LED / Debug / Graph       */
/*      reads the same shape regardless of which encoding path drove   */
/*      the click.                                                     */
/*                                                                     */
/*  Consequence: a node wired downstream of the output only sees       */
/*  *user-driven* state changes, not external ones (a bulb toggled     */
/*  from another flow or the Z2M frontend).  To shadow external        */
/*  changes too, wire the observer off a separate branch from the      */
/*  same #PnMqtt source.                                               */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ZIGBEE_SWITCH (pn_zigbee_switch_get_type ())

G_DECLARE_FINAL_TYPE (PnZigbeeSwitch, pn_zigbee_switch,
                      PN, ZIGBEE_SWITCH, PnSwitch)

PnZigbeeSwitch *pn_zigbee_switch_new (void);

G_END_DECLS

#endif /* PN_ZIGBEE_SWITCH_H */
