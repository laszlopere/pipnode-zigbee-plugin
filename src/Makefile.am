# The loadable module.  Installs into the directory the host scans at
# start-up ($(PIPNODE_PLUGINDIR), pulled from pipnode-core.pc by
# configure), so it is picked up without setting PIPNODE_PLUGIN_PATH.
#
# -module        : a loadable module, not a library (no `lib` prefix).
# -avoid-version : plain pipnode_zigbee.so, no SONAME .0.0.0 suffix that
#                  g_module_open would fail to find.
# -shared        : never emit a static archive.
plugindir = $(PIPNODE_PLUGINDIR)

plugin_LTLIBRARIES = pipnode_zigbee.la

pipnode_zigbee_la_SOURCES = \
	pn-zigbee-plugin.c \
	pn-zigbee-switch.c \
	pn-zigbee-switch.h \
	pn-znp-ping.c \
	pn-znp-ping.h

pipnode_zigbee_la_CFLAGS  = $(PIPNODE_CFLAGS)
pipnode_zigbee_la_LDFLAGS = -module -avoid-version -shared
pipnode_zigbee_la_LIBADD  = $(PIPNODE_LIBS)

# Per-node HTML help pages, installed into the directory the help browser
# scans ($(PIPNODE_HELPDIR), from pipnode-core.pc).
pnhelpdir   = $(PIPNODE_HELPDIR)
pnhelp_DATA = \
	help/PnZigbeeSwitch.html \
	help/PnZnpPing.html
EXTRA_DIST  = $(pnhelp_DATA)
