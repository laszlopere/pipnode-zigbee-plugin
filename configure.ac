#                                               -*- Autoconf -*-
# Out-of-tree pipnode plugin: Zigbee control / query nodes.
#
# Builds a single GTK-free loadable module (pipnode_zigbee.so) against
# the installed pipnode-core library and installs it into the directory
# the host scans at start-up.  No GTK: the nodes here drive Zigbee
# devices via a Zigbee2MQTT instance over MQTT (the dongle itself is a
# Zigbee radio, not an MQTT endpoint -- see README.md), so the plugin
# is server-installable and runs under pipnode-run headless.

AC_PREREQ([2.69])
AC_INIT([pipnode-zigbee-plugin], [0.1.0-dev], [laszlopere@gmail.com],
        [pipnode-zigbee-plugin], [https://github.com/laszlopere/pipnode-zigbee-plugin])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_SRCDIR([src/pn-zigbee-plugin.c])

AM_INIT_AUTOMAKE([1.16 foreign subdir-objects -Wall -Werror])
AM_SILENT_RULES([yes])

# Loadable modules: shared only, no static archive.
AC_PROG_CC
AM_PROG_AR
LT_INIT([disable-static])
PKG_PROG_PKG_CONFIG

# Host library.  We deliberately Require pipnode-core (the GTK-free tier)
# rather than the `pipnode` GUI meta-package: a Zigbee node is pure
# logic.  pipnode-core transitively provides glib/gobject/gio, json-glib
# and gmodule, so PIPNODE_CFLAGS / PIPNODE_LIBS is the whole compile and
# link line a node needs.
PKG_CHECK_MODULES([PIPNODE], [pipnode-core >= 0.2.0 gmodule-2.0])

# Pull the host's canonical install directories straight out of
# pipnode-core.pc so we never hard-code $libdir/pipnode/plugins.  These
# are the directories the host actually scans at run-time (see the
# PLUGINS guide in the pipnode tree, section 7).
PIPNODE_PLUGINDIR=`$PKG_CONFIG --variable=plugindir pipnode-core`
AS_IF([test -z "$PIPNODE_PLUGINDIR"],
    [AC_MSG_ERROR([pipnode-core.pc does not expose a 'plugindir' variable; pipnode is too old])])
AC_SUBST([PIPNODE_PLUGINDIR])

PIPNODE_HELPDIR=`$PKG_CONFIG --variable=helpdir pipnode-core`
AC_SUBST([PIPNODE_HELPDIR])

# pipnode-core.pc exposes no dedicated 'examplesdir', so derive it from the
# host's datadir: example worksheets live beside the help pages, at
# $datadir/pipnode/examples/<category>/ (the dir the editor scans, see the
# host's examples/Makefile.am).
PIPNODE_DATADIR=`$PKG_CONFIG --variable=datadir pipnode-core`
AS_IF([test -z "$PIPNODE_DATADIR"],
    [AC_MSG_ERROR([pipnode-core.pc does not expose a 'datadir' variable; pipnode is too old])])
PIPNODE_EXAMPLESDIR="$PIPNODE_DATADIR/pipnode/examples"
AC_SUBST([PIPNODE_EXAMPLESDIR])

AC_MSG_NOTICE([pipnode plugin will install into $PIPNODE_PLUGINDIR])

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    examples/Makefile
])
AC_OUTPUT

AC_MSG_NOTICE([

  $PACKAGE_NAME $PACKAGE_VERSION configured.

    plugin install dir   : $PIPNODE_PLUGINDIR
    help install dir     : $PIPNODE_HELPDIR
    examples install dir : $PIPNODE_EXAMPLESDIR

  Now run:  make && sudo make install
])
