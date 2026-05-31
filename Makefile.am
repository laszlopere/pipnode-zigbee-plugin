ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src examples

# Bundled menu icon (Apache-2.0 Material Design Icons "zigbee" glyph,
# see data/icons/README.md).  Installed into $(zigbeeicondir); the GUI
# module's pn_device_provider_register() points at its absolute path.
zigbeeicondir        = @zigbeeicondir@
dist_zigbeeicon_DATA = data/icons/zigbee.svg

EXTRA_DIST = \
	autogen.sh \
	README.md \
	LICENSE \
	data/icons/README.md
