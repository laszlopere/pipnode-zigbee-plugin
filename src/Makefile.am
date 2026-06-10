# The loadable module.  Installs into the directory the host scans at
# start-up ($(PIPNODE_PLUGINDIR), pulled from pipnode-core.pc by
# configure), so it is picked up without setting PIPNODE_PLUGIN_PATH.
#
# -module        : a loadable module, not a library (no `lib` prefix).
# -avoid-version : plain pipnode_zigbee.so, no SONAME .0.0.0 suffix that
#                  g_module_open would fail to find.
# -shared        : never emit a static archive.
plugindir = $(PIPNODE_PLUGINDIR)

plugin_LTLIBRARIES = pipnode_zigbee.la pipnode_zigbee-gui.la

pipnode_zigbee_la_SOURCES = \
	pn-zigbee-plugin.c \
	pn-zigbee-source.c \
	pn-zigbee-source.h \
	pn-zigbee-sink.c \
	pn-zigbee-sink.h \
	pn-zigbee-switch.c \
	pn-zigbee-switch.h \
	pn-zigbee-relay-status.c \
	pn-zigbee-relay-status.h \
	pn-zigbee-relay-command.c \
	pn-zigbee-relay-command.h \
	pn-zigbee-remote.c \
	pn-zigbee-remote.h \
	pn-zigbee-button.c \
	pn-zigbee-button.h \
	pn-zigbee-water-leak.c \
	pn-zigbee-water-leak.h \
	pn-zigbee-permit-join.c \
	pn-zigbee-permit-join.h

pipnode_zigbee_la_CFLAGS  = $(PIPNODE_CFLAGS)
pipnode_zigbee_la_LDFLAGS = -module -avoid-version -shared
pipnode_zigbee_la_LIBADD  = $(PIPNODE_LIBS)

# GUI companion module.  Automake canonicalises the `pipnode_zigbee-gui.la`
# target to the `pipnode_zigbee_gui_la_` variable prefix.  It links the
# GTK tier (pipnode-gui pulls in libpipnode-gui + libpipnode-core + GTK),
# exports pn_plugin_gui_init(), and registers the "Zigbee" Devices-menu
# provider.  Same -module/-avoid-version/-shared flags as the logic half.
pipnode_zigbee_gui_la_SOURCES = pn-zigbee-gui.c
# ZB_ICON_PATH: the absolute path of the installed menu icon, handed to
# pn_device_provider_register() (the host now accepts a full path).  make
# expands $(datadir) here, so the compiled-in string is fully resolved.
pipnode_zigbee_gui_la_CFLAGS  = $(PIPNODE_GUI_CFLAGS) \
	-DZB_ICON_PATH=\"@zigbeeicondir@/zigbee.svg\"
pipnode_zigbee_gui_la_LDFLAGS = -module -avoid-version -shared
pipnode_zigbee_gui_la_LIBADD  = $(PIPNODE_GUI_LIBS)

# Per-node HTML help pages, installed into the directory the help browser
# scans ($(PIPNODE_HELPDIR), from pipnode-core.pc).
pnhelpdir   = $(PIPNODE_HELPDIR)
pnhelp_DATA = \
	help/PnZigbeeSource.html \
	help/PnZigbeeSink.html \
	help/PnZigbeeSwitch.html \
	help/PnZigbeeRelayStatus.html \
	help/PnZigbeeRelayCommand.html \
	help/PnZigbeeRemote.html \
	help/PnZigbeeButton.html \
	help/PnZigbeeWaterLeak.html \
	help/PnZigbeePermitJoin.html
EXTRA_DIST  = $(pnhelp_DATA)
