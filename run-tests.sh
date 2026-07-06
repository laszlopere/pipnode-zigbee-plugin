#!/usr/bin/env bash
#
# Build and run the Zigbee plugin's unit tests.
#
# Each test is a small standalone binary that links the node under test
# straight against the installed pipnode-core (located via pkg-config);
# no autotools wiring and no installed plugin .so are needed.  The tests
# are headless and never pump a GLib main loop, so they open no serial
# port, make no MQTT connection, and touch no real filesystem.
#
# Usage:
#   ./run-tests.sh [-v|--verbose] [-k|--keep] [TEST ...]
#
#   -v, --verbose   pass through to the test binaries (per-check trace)
#   -k, --keep      keep the built binaries under tests/.build afterwards
#   TEST            run only the named test(s), e.g. "relay-command"
#                   (matched against the test file basename); default all
#
# Exit status is non-zero if any test binary reports a failed check or
# fails to build.
#
# Copyright (C) 2026 Laszlo Pere
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/src"
tests="$here/tests"
build="$tests/.build"

verbose=""
keep=0
filters=()

while [ $# -gt 0 ]; do
    case "$1" in
        -v|--verbose) verbose="-v" ;;
        -k|--keep)    keep=1 ;;
        -h|--help)
            sed -n '3,28p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        -*)
            echo "unknown option: $1" >&2
            exit 2 ;;
        *)
            filters+=("$1") ;;
    esac
    shift
done

# pipnode-core supplies glib/gobject/gio/json-glib transitively; gmodule
# is pulled in the same way the plugin's configure.ac does.
pkgs="pipnode-core gmodule-2.0"
if ! pkg-config --exists $pkgs; then
    echo "error: $pkgs not found by pkg-config." >&2
    echo "       Install pipnode (with developer files) first." >&2
    exit 1
fi

cflags="$(pkg-config --cflags $pkgs)"
libs="$(pkg-config --libs $pkgs)"
cc="${CC:-cc}"

# Each test binary = its own .c + the harness + the single node source it
# exercises.  Keep this table in sync when a node is added.
#
# test-zigbee-source drives PnZigbeeSource's accept_topic / process_message
# vfuncs directly (PN_MQTT_GET_CLASS), with no main loop.  PnZigbeeSource
# subclasses PnMqtt, but the host debounces the connect onto a main-loop
# idle (pn-mqtt.c's schedule_restart -> g_idle_add), so a test that never
# pumps a loop opens no socket -- merely constructing one is network-free.
#
# This was blocked for a while on an older INSTALLED pipnode-core that
# connected SYNCHRONOUSLY from construction (it would hit the live broker).
# Resolved 2026-05-31: the installed pipnode-core now has the idle-debounced
# connect.  Re-verify network-free after a host upgrade with:
#   strace -f -e trace=network <built test>  # must show no connect() to :1883
#
#   <test-basename>            <node source under src/>
tests_table=(
    "test-zigbee-relay-status   pn-zigbee-relay-status.c"
    "test-zigbee-relay-command  pn-zigbee-relay-command.c"
    "test-zigbee-remote         pn-zigbee-remote.c"
    "test-zigbee-water-leak     pn-zigbee-water-leak.c"
    "test-zigbee-switch         pn-zigbee-switch.c"
    "test-zigbee-permit-join    pn-zigbee-permit-join.c"
    "test-zigbee-source         pn-zigbee-source.c"
    "test-zigbee-sink           pn-zigbee-sink.c"
    "test-zigbee-name-registry  pn-zigbee-name-registry.c"
)

selected () {
    # No filters -> everything selected.
    [ ${#filters[@]} -eq 0 ] && return 0
    local name="$1" f
    for f in "${filters[@]}"; do
        case "$name" in *"$f"*) return 0 ;; esac
    done
    return 1
}

mkdir -p "$build"

ran=0
passed=0
failed=0
build_errors=0

for row in "${tests_table[@]}"; do
    name="${row%% *}"
    node="${row##* }"

    selected "$name" || continue

    bin="$build/$name"
    if ! "$cc" -g -I"$tests" -I"$src" $cflags \
            "$tests/$name.c" "$tests/pn-test.c" "$src/$node" \
            $libs -lm -o "$bin" 2>"$build/$name.build.log"; then
        echo "BUILD FAILED: $name" >&2
        cat "$build/$name.build.log" >&2
        build_errors=$((build_errors + 1))
        continue
    fi

    ran=$((ran + 1))
    if "$bin" $verbose; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
    echo
done

echo "=================================================="
echo "suites: $ran run, $passed passed, $failed failed${build_errors:+, $build_errors build errors}"

if [ "$keep" -eq 0 ]; then
    rm -rf "$build"
fi

[ "$failed" -eq 0 ] && [ "$build_errors" -eq 0 ]
