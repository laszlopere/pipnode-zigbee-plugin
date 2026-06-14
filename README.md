# pipnode-zigbee-plugin

[![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
![Language: C](https://img.shields.io/badge/language-C-555.svg)
[![Sponsor](https://img.shields.io/badge/Sponsor-%E2%9D%A4-db61a2.svg)](https://github.com/sponsors/laszlopere)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-555.svg)
[![Last commit](https://img.shields.io/github/last-commit/laszlopere/pipnode-zigbee-plugin.svg)](https://github.com/laszlopere/pipnode-zigbee-plugin/commits)

**pipnode-zigbee-plugin** adds Zigbee control and query nodes to
[pipnode](https://github.com/laszlopere/pipnode), driving your devices
through a [Zigbee2MQTT](https://www.zigbee2mqtt.io/) bridge over MQTT.

[pipnode](https://github.com/laszlopere/pipnode) is the host application
this plugin extends — a node-based worksheet tool for wiring up
home-automation and data flows. See the
[pipnode repository](https://github.com/laszlopere/pipnode) for the host
itself, how to install it, and its plugin interface. This plugin loads
into pipnode through that documented interface and contributes Z2M-aware
node types — the right topic shapes, payload schemas, and device
discovery — on top of pipnode's generic MQTT primitives.

![A Zigbee remote wired to a Zigbee switch on a pipnode worksheet](screenshots/remote-switch.png)

![A Zigbee button driving two plugs on a pipnode worksheet](screenshots/button.png)

![The Zigbee Devices dialog listing devices discovered from the Zigbee2MQTT bridge](screenshots/zigbee.devices.png)

> **Status: in progress.** The following node types are registered and
> usable: `PnZigbeeSource`, `PnZigbeeSwitch`, `PnZigbeeRelayStatus`,
> `PnZigbeeRelayCommand`, `PnZigbeeRemote`, and `PnZigbeeWaterLeak`. More
> (ZigbeeEvent, ZigbeePair, ZigbeeBridgeStatus, …) will be added
> incrementally.

This is **free software**, licensed under the **GNU GPL v3 or later**
(see [LICENSE](LICENSE) / [COPYING](COPYING)). It talks to the pipnode
host only through the documented plugin interface, so the combination is
covered by pipnode's `LICENSE.PLUGIN-EXCEPTION` (an additional permission
under GPL v3 §7); this plugin's own source and binaries are GPLv3. Pipnode
itself remains GPLv3-or-later — relicensing this plugin does not change
that.

## Building

Requires an installed pipnode with developer files (`pipnode-core.pc` on
the `pkg-config` path, headers under `$prefix/include/pipnode/`).

```sh
./autogen.sh           # only after a fresh git clone
./configure
make
sudo make install      # installs into pipnode's plugin dir
```

`configure` reads the install location straight from `pipnode-core.pc`
(`pkg-config --variable=plugindir pipnode-core`), so the module lands in
the directory the host scans at start-up — typically
`/usr/local/lib/pipnode/plugins/`.

### Per-user install (no sudo)

To test without installing system-wide, build and point pipnode at the
build tree:

```sh
make
PIPNODE_PLUGIN_PATH=$PWD/src/.libs pipnode
```

## Sponsorship

This plugin is free and open source, and so is
[pipnode](https://github.com/laszlopere/pipnode) itself. If they are useful
to you, please consider sponsoring on
[**GitHub Sponsors**](https://github.com/sponsors/laszlopere). Sponsoring at
any tier funds continued development of the open-source pipnode ecosystem
for everyone.

## License

Copyright (C) 2026 Laszlo Pere. Licensed under the GNU General Public
License, version 3 or later — see [LICENSE](LICENSE) / [COPYING](COPYING).
The plugin reaches the pipnode host only through the documented plugin
interface, so the combination is permitted by pipnode's
`LICENSE.PLUGIN-EXCEPTION`; pipnode's own core stays GPLv3-or-later.
