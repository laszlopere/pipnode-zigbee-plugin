# TODO

## Adopt pipnode's enhanced error-reporting / logging facilities (PLUGINS §12)

The host now offers a three-channel failure-reporting contract that nodes
are expected to use (all GTK-free, available headless):

1. **Downstream** — emit a `PnMessage` with `success=FALSE` + human-readable
   `output`.
2. **Visual** — `pn_node_set_has_error (node, TRUE/FALSE)` (`pn-node.h:708`);
   the host paints the red body + ❗ itself. PLUGINS §12 explicitly forbids
   hand-rolling that swap via `pn_node_set_color` / `pn_node_set_icon`.
3. **Diagnostics log** — `pn_node_log()` / `pn_node_log_info|warning|error()`
   (`pn-node.h:726-754`) instead of stdout/stderr or silent drops.

Audit (2026-05-29): no node is fully migrated. `pn-zigbee-remote.c` is the
only one that uses `pn_node_log_*` at all.

### Channel 2 — replace hand-rolled red+❗ swap with `pn_node_set_has_error`

- [ ] `src/pn-znp-ping.c:108-110` — `apply_visual_state` swaps the warning
      glyph via `pn_node_set_icon`. The dead state is unambiguous transient
      runtime failure, so this is a clear migration: `pn_node_set_has_error
      (node, !alive)`. (Callers/seed: `:343-344, :366-367, :443, :508, :724`.)
- [ ] `src/pn-zigbee-switch.c:93-95` — `apply_visual_state` red+❗ branch.
- [ ] `src/pn-zigbee-relay-status.c:85-87` — same pattern.
- [ ] `src/pn-zigbee-relay-command.c:83-85` — same pattern (and this node
      fires physical relays with no error indication on failure).

      Note: the four zigbee swaps signal *unconfigured* (empty friendly-name),
      not transient runtime errors. §12's `set_has_error` is framed as
      transient error state, so migrating these is a judgment call — decide
      whether "unconfigured" should count as node error before touching them.

### Channel 3 — adopt `pn_node_log_*` for diagnostics

- [ ] `src/pn-znp-ping.c` — highest value: `emit_failure` (`:118`) and the
      paths that build rich reason strings (serial open `:441`, timeout `:507`,
      write `:523,537`, HUP `:343`, `g_strerror(errno)` `:366`) only travel
      downstream; also feed them to `pn_node_log_error`.
- [ ] `src/pn-zigbee-source.c` — silent JSON-drop / fallback paths
      (`:590, :617`) could log a WARNING via `pn_node_log_warning`.
- [ ] `src/pn-zigbee-switch.c` — silent drops in `receive`
      (`:255, :259, :263, :275`) and fallback publish (`:134`).
- [ ] `src/pn-zigbee-relay-status.c` — silent drops in `receive`
      (`:181, :185, :189, :202`).
- [ ] `src/pn-zigbee-relay-command.c` — silent drops in `receive`
      (`:152, :158`).

`pn-zigbee-remote.c` already uses `pn_node_log_info` on each press (`:247`);
only outstanding item there is the Channel-2 `set_has_error` adoption.
