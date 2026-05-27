/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#ifndef PN_ZNP_PING_H
#define PN_ZNP_PING_H

#include <pn-node.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZnpPing                                                          */
/*                                                                     */
/*  Primitive liveness/diagnostic node for a TI CC2652/CC1352 ZNP      */
/*  coordinator dongle (e.g. AVATTO GW70-MQTT) attached over USB       */
/*  serial.  Speaks the Z-Stack ZNP UART protocol natively in C: at    */
/*  a configurable interval it sends SYS_PING (and, on the first       */
/*  successful round-trip, SYS_VERSION) over UNPI framing and emits    */
/*  the parsed response as a #PnMessage on its output.  Any            */
/*  unsolicited frame the coordinator sends in between (e.g. boot      */
/*  banners, ZDO indications if the network is up) is emitted          */
/*  verbatim as a separate "frame" message -- enough to monitor "is    */
/*  this thing alive and what is it saying" without a Zigbee2MQTT      */
/*  layer.                                                             */
/*                                                                     */
/*  Owns the serial fd exclusively for the node's lifetime (flock +    */
/*  TIOCEXCL); a second instance pointed at the same path fails        */
/*  with EBUSY.  That single-owner limitation is the same one the      */
/*  Meshtastic node hits and is the motivation for TODO #28 in the    */
/*  pipnode tree (host-managed shared profile sessions, ABI v6).       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ZNP_PING (pn_znp_ping_get_type ())

G_DECLARE_FINAL_TYPE (PnZnpPing, pn_znp_ping, PN, ZNP_PING, PnNode)

PnZnpPing *pn_znp_ping_new (void);

G_END_DECLS

#endif /* PN_ZNP_PING_H */
