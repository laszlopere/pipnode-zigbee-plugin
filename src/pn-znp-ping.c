/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#include "pn-znp-ping.h"

#include <pn-message.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>

/* ------------------------------------------------------------------ */
/*  TI Z-Stack ZNP / UNPI protocol constants                           */
/*                                                                     */
/*  Frame:  SOF(0xFE) | LEN(1) | CMD0(1) | CMD1(1) | DATA(LEN) | FCS  */
/*  FCS  =  XOR of LEN, CMD0, CMD1, and every DATA byte.               */
/*  LEN  is the data length only (0..250).                             */
/*                                                                     */
/*  CMD0 packs (type<<5) | subsystem:                                  */
/*      type:   POLL=0x00, SREQ=0x20, AREQ=0x40, SRSP=0x60             */
/*      sub:    SYS=0x01, AF=0x04, ZDO=0x05, UTIL=0x07, ...            */
/*  So SYS_SREQ = 0x21, SYS_SRSP = 0x61.                               */
/* ------------------------------------------------------------------ */

#define ZNP_SOF              0xFE
#define ZNP_MAX_DATA         250

#define ZNP_TYPE_MASK        0xE0
#define ZNP_SUBSYS_MASK      0x1F
#define ZNP_TYPE_SREQ        0x20
#define ZNP_TYPE_SRSP        0x60
#define ZNP_TYPE_AREQ        0x40
#define ZNP_SUBSYS_SYS       0x01

#define ZNP_CMD0_SYS_SREQ    (ZNP_TYPE_SREQ | ZNP_SUBSYS_SYS)  /* 0x21 */
#define ZNP_CMD0_SYS_SRSP    (ZNP_TYPE_SRSP | ZNP_SUBSYS_SYS)  /* 0x61 */

#define ZNP_SYS_PING         0x01
#define ZNP_SYS_VERSION      0x02

/* Visual: FA4.7 fa-feed U+F09E (broadcast/radio).  The dead state is
 * flagged with the host's generic has-error overlay (red body + ❗),
 * not a hand-rolled icon swap -- see PLUGINS §12. */
#define PN_ZNP_PING_NORMAL_ICON  "\xef\x82\x9e"  /* U+F09E */

#define PN_ZNP_PING_DEFAULT_DEVICE   "/dev/ttyUSB0"
#define PN_ZNP_PING_DEFAULT_INTERVAL 5u
#define PN_ZNP_PING_REPLY_TIMEOUT_MS 800
#define PN_ZNP_PING_RX_BUFFER       1024

enum
{
    PROP_0,
    PROP_DEVICE,
    PROP_INTERVAL,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

struct _PnZnpPing
{
    PnNode parent_instance;

    gchar       *device;
    guint        interval;       /* seconds; 0 disables polling */

    int          fd;             /* -1 when closed */
    GIOChannel  *channel;
    guint        io_watch_id;
    guint        poll_source_id;

    guint8       rxbuf[PN_ZNP_PING_RX_BUFFER];
    gsize        rxlen;

    gboolean     waiting_for_ping;
    gboolean     waiting_for_version;
    gboolean     version_emitted;    /* per device-open, single-shot */
    gint64       request_sent_at_us;

    gboolean     last_alive;     /* drives the visual state */
};

G_DEFINE_TYPE (PnZnpPing, pn_znp_ping, PN_TYPE_NODE)

static void open_device   (PnZnpPing *self);
static void close_device  (PnZnpPing *self);
static void schedule_poll (PnZnpPing *self);
static void cancel_poll   (PnZnpPing *self);
static void send_ping     (PnZnpPing *self);
static void send_version  (PnZnpPing *self);

/* ------------------------------------------------------------------ */
/*  Visual                                                             */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (PnZnpPing *self, gboolean alive)
{
    PnNode  *node    = PN_NODE (self);
    PnColor  magenta = { 0.78, 0.27, 0.60, 1.0 };

    self->last_alive = alive;

    /* Keep the healthy Zigbee identity (magenta + radio glyph) set
     * unconditionally; the host paints the red ❗ overlay while
     * has-error is set, so a dead dongle needs no swap of our own
     * (PLUGINS §12).  Seeding both here also gives the instance a
     * non-grey body -- pn_node_get_color() has no class fallback. */
    pn_node_set_color (node, &magenta);
    pn_node_set_icon  (node, PN_ZNP_PING_NORMAL_ICON);
    pn_node_set_has_error (node, !alive);
}

/* ------------------------------------------------------------------ */
/*  Message helpers                                                    */
/* ------------------------------------------------------------------ */

static void
emit_failure (PnZnpPing *self, const gchar *reason)
{
    PnMessage *msg = pn_message_new (PN_NODE (self), NULL);
    pn_message_set_string  (msg, "topic",   "znp");
    pn_message_set_boolean (msg, "success", FALSE);
    pn_message_set_string  (msg, "output",  reason);
    pn_message_set_string  (msg, "device",  self->device ? self->device : "");
    pn_node_emit_message (PN_NODE (self), msg);
    g_object_unref (msg);

    /* Every failure path (serial open, reply timeout, write, HUP/ERR,
     * read errno) funnels through here, so mirroring the reason into
     * the node's diagnostic log once covers them all -- the downstream
     * failure message alone is invisible under a desktop launcher
     * (PLUGINS §12, channel 3). */
    pn_node_log_error (PN_NODE (self), "%s", reason);
}

static gchar *
hexdump_data (const guint8 *data, gsize len)
{
    if (len == 0)
        return g_strdup ("");

    GString *s = g_string_sized_new (len * 3);
    for (gsize i = 0; i < len; ++i)
        g_string_append_printf (s, i ? " %02X" : "%02X", data[i]);
    return g_string_free (s, FALSE);
}

static void
emit_unsolicited_frame (PnZnpPing  *self,
                        guint8      cmd0,
                        guint8      cmd1,
                        const guint8 *data,
                        gsize       len)
{
    PnMessage *msg = pn_message_new (PN_NODE (self), NULL);
    gchar *hex = hexdump_data (data, len);

    pn_message_set_string  (msg, "topic",      "znp/frame");
    pn_message_set_boolean (msg, "success",    TRUE);
    pn_message_set_int     (msg, "cmd0",       cmd0);
    pn_message_set_int     (msg, "cmd1",       cmd1);
    pn_message_set_int     (msg, "subsystem",  cmd0 & ZNP_SUBSYS_MASK);
    pn_message_set_int     (msg, "type",       cmd0 & ZNP_TYPE_MASK);
    pn_message_set_int     (msg, "length",     (gint) len);
    pn_message_set_string  (msg, "data_hex",   hex);
    pn_message_set_string  (msg, "output",     hex);
    pn_message_set_string  (msg, "device",     self->device ? self->device : "");

    pn_node_emit_message (PN_NODE (self), msg);
    g_object_unref (msg);
    g_free (hex);
}

static void
emit_ping_ok (PnZnpPing *self, guint16 capabilities)
{
    PnMessage *msg = pn_message_new (PN_NODE (self), NULL);
    gchar *summary = g_strdup_printf ("PING ok, capabilities 0x%04X",
                                      capabilities);
    pn_message_set_string  (msg, "topic",        "znp/ping");
    pn_message_set_boolean (msg, "success",      TRUE);
    pn_message_set_int     (msg, "capabilities", capabilities);
    pn_message_set_string  (msg, "output",       summary);
    pn_message_set_string  (msg, "device",       self->device ? self->device : "");
    pn_node_emit_message (PN_NODE (self), msg);
    g_object_unref (msg);
    g_free (summary);

    if (!self->last_alive)
        apply_visual_state (self, TRUE);
}

static void
emit_version (PnZnpPing *self, const guint8 *d, gsize len)
{
    if (len < 9)
        return;  /* malformed; ignore */

    guint8  transport = d[0];
    guint8  product   = d[1];
    guint8  major     = d[2];
    guint8  minor     = d[3];
    guint8  maint     = d[4];
    guint32 revision  = (guint32) d[5]
                      | ((guint32) d[6] << 8)
                      | ((guint32) d[7] << 16)
                      | ((guint32) d[8] << 24);

    PnMessage *msg = pn_message_new (PN_NODE (self), NULL);
    gchar *summary = g_strdup_printf (
            "Z-Stack %u.%u.%u  (transport %u, product %u, build %u)",
            major, minor, maint, transport, product, revision);

    pn_message_set_string  (msg, "topic",        "znp/version");
    pn_message_set_boolean (msg, "success",      TRUE);
    pn_message_set_int     (msg, "transport",    transport);
    pn_message_set_int     (msg, "product",      product);
    pn_message_set_int     (msg, "major",        major);
    pn_message_set_int     (msg, "minor",        minor);
    pn_message_set_int     (msg, "maintenance",  maint);
    pn_message_set_int64   (msg, "revision",     revision);
    pn_message_set_string  (msg, "output",       summary);
    pn_message_set_string  (msg, "device",       self->device ? self->device : "");
    pn_node_emit_message (PN_NODE (self), msg);
    g_object_unref (msg);
    g_free (summary);
}

/* ------------------------------------------------------------------ */
/*  UNPI framing                                                       */
/* ------------------------------------------------------------------ */

static gboolean
write_frame (PnZnpPing  *self,
             guint8      cmd0,
             guint8      cmd1,
             const guint8 *data,
             guint8      len)
{
    if (self->fd < 0)
        return FALSE;

    guint8 buf[5 + ZNP_MAX_DATA];
    buf[0] = ZNP_SOF;
    buf[1] = len;
    buf[2] = cmd0;
    buf[3] = cmd1;
    if (len > 0)
        memcpy (buf + 4, data, len);

    guint8 fcs = len ^ cmd0 ^ cmd1;
    for (guint8 i = 0; i < len; ++i)
        fcs ^= data[i];
    buf[4 + len] = fcs;

    gsize total   = 5 + len;
    gsize written = 0;
    while (written < total)
    {
        ssize_t n = write (self->fd, buf + written, total - written);
        if (n > 0) { written += n; continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return FALSE;
    }
    return TRUE;
}

static void
consume_buffer (PnZnpPing *self)
{
    /* Slide a parse cursor across self->rxbuf, dispatching every
     * complete frame found.  Anything before the first SOF byte is
     * discarded as line noise. */
    gsize r = 0;
    while (r < self->rxlen)
    {
        if (self->rxbuf[r] != ZNP_SOF) { r++; continue; }

        /* Need at least SOF + LEN + CMD0 + CMD1 + FCS = 5 bytes. */
        if (self->rxlen - r < 5) break;
        guint8 len  = self->rxbuf[r + 1];
        if (len > ZNP_MAX_DATA) { r++; continue; }  /* resync */

        gsize frame_size = 5 + (gsize) len;
        if (self->rxlen - r < frame_size) break;    /* wait for more */

        guint8 cmd0 = self->rxbuf[r + 2];
        guint8 cmd1 = self->rxbuf[r + 3];
        const guint8 *data = self->rxbuf + r + 4;
        guint8 fcs  = self->rxbuf[r + 4 + len];

        guint8 calc = len ^ cmd0 ^ cmd1;
        for (guint8 i = 0; i < len; ++i) calc ^= data[i];

        if (calc != fcs)
        {
            /* Bad FCS: drop the SOF and rescan; eventually we resync. */
            r++;
            continue;
        }

        /* Dispatch. */
        if (cmd0 == ZNP_CMD0_SYS_SRSP && cmd1 == ZNP_SYS_PING
            && self->waiting_for_ping && len >= 2)
        {
            self->waiting_for_ping = FALSE;
            guint16 caps = (guint16) data[0] | ((guint16) data[1] << 8);
            emit_ping_ok (self, caps);
            if (!self->version_emitted)
                send_version (self);
        }
        else if (cmd0 == ZNP_CMD0_SYS_SRSP && cmd1 == ZNP_SYS_VERSION
                 && self->waiting_for_version)
        {
            self->waiting_for_version = FALSE;
            self->version_emitted     = TRUE;
            emit_version (self, data, len);
        }
        else
        {
            /* Anything we didn't ask for, or unmatched SRSPs, gets
             * fanned out as a raw frame for monitoring. */
            emit_unsolicited_frame (self, cmd0, cmd1, data, len);
        }

        r += frame_size;
    }

    /* Shift any unconsumed bytes back to the start of the buffer. */
    if (r > 0)
    {
        if (r < self->rxlen)
            memmove (self->rxbuf, self->rxbuf + r, self->rxlen - r);
        self->rxlen -= r;
    }
}

/* ------------------------------------------------------------------ */
/*  GIOChannel watch                                                   */
/* ------------------------------------------------------------------ */

static gboolean
on_serial_readable (GIOChannel *channel, GIOCondition cond, gpointer data)
{
    PnZnpPing *self = data;
    (void) channel;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
    {
        emit_failure (self, "serial port closed (HUP/ERR)");
        apply_visual_state (self, FALSE);
        close_device (self);
        self->io_watch_id = 0;
        return G_SOURCE_REMOVE;
    }

    gsize space = sizeof (self->rxbuf) - self->rxlen;
    if (space == 0)
    {
        /* Buffer full of garbage with no SOF found.  Reset and drop. */
        self->rxlen = 0;
        space = sizeof (self->rxbuf);
    }

    ssize_t n = read (self->fd, self->rxbuf + self->rxlen, space);
    if (n > 0)
    {
        self->rxlen += (gsize) n;
        consume_buffer (self);
    }
    else if (n < 0 && errno != EINTR && errno != EAGAIN)
    {
        emit_failure (self, g_strerror (errno));
        apply_visual_state (self, FALSE);
        close_device (self);
        self->io_watch_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/*  Serial open / close                                                */
/* ------------------------------------------------------------------ */

static int
open_serial (const gchar *path, GError **error)
{
    int fd = open (path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "open(%s): %s", path, g_strerror (errno));
        return -1;
    }
    if (flock (fd, LOCK_EX | LOCK_NB) != 0)
    {
        int saved = (errno == EWOULDBLOCK) ? EBUSY : errno;
        close (fd);
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved),
                     "flock(%s): %s (another process holds the port?)",
                     path, g_strerror (saved));
        return -1;
    }
    ioctl (fd, TIOCEXCL);

    struct termios t;
    if (tcgetattr (fd, &t) != 0)
    {
        int saved = errno;
        close (fd);
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved),
                     "tcgetattr: %s", g_strerror (saved));
        return -1;
    }
    cfmakeraw (&t);
    cfsetispeed (&t, B115200);
    cfsetospeed (&t, B115200);
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    t.c_cflag &= ~CSIZE;
    t.c_cflag |= CS8;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr (fd, TCSANOW, &t) != 0)
    {
        int saved = errno;
        close (fd);
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved),
                     "tcsetattr: %s", g_strerror (saved));
        return -1;
    }
    tcflush (fd, TCIOFLUSH);
    return fd;
}

static void
open_device (PnZnpPing *self)
{
    if (self->fd >= 0) return;
    if (!self->device || !*self->device) return;

    GError *error = NULL;
    int fd = open_serial (self->device, &error);
    if (fd < 0)
    {
        emit_failure (self, error ? error->message : "open failed");
        if (error) g_error_free (error);
        apply_visual_state (self, FALSE);
        return;
    }

    self->fd      = fd;
    self->channel = g_io_channel_unix_new (fd);
    g_io_channel_set_encoding (self->channel, NULL, NULL);
    g_io_channel_set_buffered (self->channel, FALSE);
    self->io_watch_id = g_io_add_watch (self->channel,
                                        G_IO_IN | G_IO_HUP | G_IO_ERR,
                                        on_serial_readable, self);
    self->rxlen = 0;
    self->waiting_for_ping = FALSE;
    self->waiting_for_version = FALSE;
    self->version_emitted = FALSE;

    /* First contact immediately, then on the configured cadence. */
    send_ping (self);
    schedule_poll (self);
}

static void
close_device (PnZnpPing *self)
{
    cancel_poll (self);

    if (self->io_watch_id)
    {
        g_source_remove (self->io_watch_id);
        self->io_watch_id = 0;
    }
    if (self->channel)
    {
        g_io_channel_unref (self->channel);
        self->channel = NULL;
    }
    if (self->fd >= 0)
    {
        close (self->fd);
        self->fd = -1;
    }
    self->rxlen = 0;
    self->waiting_for_ping = FALSE;
    self->waiting_for_version = FALSE;
    self->version_emitted = FALSE;
}

/* ------------------------------------------------------------------ */
/*  Polling                                                            */
/* ------------------------------------------------------------------ */

static void
send_ping (PnZnpPing *self)
{
    if (self->fd < 0) return;

    /* If a previous ping never got a reply, declare the dongle dead
     * (visual + failure message) and try again with this round. */
    if (self->waiting_for_ping)
    {
        gint64 now_us = g_get_monotonic_time ();
        gint64 elapsed_ms = (now_us - self->request_sent_at_us) / 1000;
        if (elapsed_ms >= PN_ZNP_PING_REPLY_TIMEOUT_MS)
        {
            emit_failure (self, "no SYS_PING response within timeout");
            apply_visual_state (self, FALSE);
            self->waiting_for_ping = FALSE;
        }
        else
        {
            /* Still inside the timeout from the previous attempt --
             * don't pile up requests. */
            return;
        }
    }

    self->waiting_for_ping = TRUE;
    self->request_sent_at_us = g_get_monotonic_time ();
    if (!write_frame (self, ZNP_CMD0_SYS_SREQ, ZNP_SYS_PING, NULL, 0))
    {
        emit_failure (self, "write(SYS_PING) failed");
        apply_visual_state (self, FALSE);
        close_device (self);
    }
}

static void
send_version (PnZnpPing *self)
{
    if (self->fd < 0) return;
    self->waiting_for_version = TRUE;
    if (!write_frame (self, ZNP_CMD0_SYS_SREQ, ZNP_SYS_VERSION, NULL, 0))
    {
        self->waiting_for_version = FALSE;
        emit_failure (self, "write(SYS_VERSION) failed");
    }
}

static gboolean
on_poll_tick (gpointer data)
{
    PnZnpPing *self = data;
    send_ping (self);
    return G_SOURCE_CONTINUE;
}

static void
schedule_poll (PnZnpPing *self)
{
    cancel_poll (self);
    if (self->interval == 0) return;
    self->poll_source_id = g_timeout_add_seconds (self->interval,
                                                  on_poll_tick, self);
}

static void
cancel_poll (PnZnpPing *self)
{
    if (self->poll_source_id)
    {
        g_source_remove (self->poll_source_id);
        self->poll_source_id = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Startup-announce + property reactions                              */
/* ------------------------------------------------------------------ */

static gboolean
startup_open_idle (gpointer data)
{
    /* Properties are loaded AFTER constructed() but BEFORE the idle
     * handler runs, so this is the safe spot to open the device with
     * whatever path the worksheet supplies.  See memory note
     * "startup-announce pattern for source nodes". */
    PnZnpPing *self = data;
    open_device (self);
    return G_SOURCE_REMOVE;
}

static void
on_device_changed (PnZnpPing *self)
{
    close_device (self);
    open_device (self);
}

static void
on_interval_changed (PnZnpPing *self)
{
    if (self->fd >= 0)
        schedule_poll (self);
}

/* ------------------------------------------------------------------ */
/*  GObject                                                            */
/* ------------------------------------------------------------------ */

static void
pn_znp_ping_get_property (GObject *object, guint pid,
                          GValue *value, GParamSpec *pspec)
{
    PnZnpPing *self = PN_ZNP_PING (object);
    switch (pid)
    {
        case PROP_DEVICE:   g_value_set_string (value, self->device);   break;
        case PROP_INTERVAL: g_value_set_uint   (value, self->interval); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, pid, pspec); break;
    }
}

static void
pn_znp_ping_set_property (GObject *object, guint pid,
                          const GValue *value, GParamSpec *pspec)
{
    PnZnpPing *self = PN_ZNP_PING (object);
    switch (pid)
    {
        case PROP_DEVICE:
        {
            const gchar *v = g_value_get_string (value);
            if (g_strcmp0 (self->device, v) == 0) return;
            g_free (self->device);
            self->device = g_strdup (v ? v : "");
            on_device_changed (self);
            break;
        }
        case PROP_INTERVAL:
        {
            guint v = g_value_get_uint (value);
            if (self->interval == v) return;
            self->interval = v;
            on_interval_changed (self);
            break;
        }
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, pid, pspec);
            break;
    }
}

static void
pn_znp_ping_dispose (GObject *object)
{
    PnZnpPing *self = PN_ZNP_PING (object);
    close_device (self);
    G_OBJECT_CLASS (pn_znp_ping_parent_class)->dispose (object);
}

static void
pn_znp_ping_finalize (GObject *object)
{
    PnZnpPing *self = PN_ZNP_PING (object);
    g_free (self->device);
    G_OBJECT_CLASS (pn_znp_ping_parent_class)->finalize (object);
}

static void
pn_znp_ping_constructed (GObject *object)
{
    G_OBJECT_CLASS (pn_znp_ping_parent_class)->constructed (object);
    /* Defer open until after the worksheet has applied saved properties
     * (notably the device path).  See pattern note above. */
    g_idle_add (startup_open_idle, object);
}

static void
pn_znp_ping_class_init (PnZnpPingClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_znp_ping_get_property;
    object_class->set_property = pn_znp_ping_set_property;
    object_class->constructed  = pn_znp_ping_constructed;
    object_class->dispose      = pn_znp_ping_dispose;
    object_class->finalize     = pn_znp_ping_finalize;

    node_class->class_name = "ZNP Ping";
    node_class->icon       = PN_ZNP_PING_NORMAL_ICON;
    node_class->color      = (PnColor){ 0.78, 0.27, 0.60, 1.0 };
    node_class->category   = "Zigbee";
    node_class->has_input  = FALSE;
    node_class->has_output = TRUE;

    props[PROP_DEVICE] = g_param_spec_string (
            "device", "Device",
            "Serial-port path of the ZNP coordinator dongle "
            "(e.g. /dev/ttyUSB0 or a /dev/serial/by-id/... symlink)",
            PN_ZNP_PING_DEFAULT_DEVICE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_INTERVAL] = g_param_spec_uint (
            "interval", "Interval",
            "Seconds between SYS_PING attempts.  0 disables polling "
            "and the node only forwards unsolicited frames.",
            0, 3600, PN_ZNP_PING_DEFAULT_INTERVAL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_znp_ping_init (PnZnpPing *self)
{
    self->device       = g_strdup (PN_ZNP_PING_DEFAULT_DEVICE);
    self->interval     = PN_ZNP_PING_DEFAULT_INTERVAL;
    self->fd           = -1;
    self->channel      = NULL;
    self->io_watch_id  = 0;
    self->poll_source_id = 0;
    self->rxlen        = 0;
    self->waiting_for_ping = FALSE;
    self->waiting_for_version = FALSE;
    self->version_emitted = FALSE;
    self->last_alive   = FALSE;

    pn_node_set_has_input  (PN_NODE (self), FALSE);
    pn_node_set_has_output (PN_NODE (self), TRUE);

    apply_visual_state (self, FALSE);
}

PnZnpPing *
pn_znp_ping_new (void)
{
    return g_object_new (PN_TYPE_ZNP_PING, NULL);
}
