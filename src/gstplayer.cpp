/* This file is part of the KMPlayer application
   Copyright (C) 2004 Koos Vriezen <koos.vriezen@xs4all.nl>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <config.h>
#include <dcopclient.h>
#include <qcstring.h>
#include <qtimer.h>
#include <qfile.h>
#include <qurl.h>
#include <qthread.h>
#include <qmutex.h>
#include <qdom.h>
#include "kmplayer_backend.h"
#include "kmplayer_callback_stub.h"
#include "kmplayer_callback.h"
#include "gstplayer.h"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <gst/gst.h>
#include <gst/xoverlay/xoverlay.h>

static char                 configfile[2048];

static Display             *display;
static KGStreamerPlayer   *gstapp;
static KMPlayerCallback_stub * callback;
static Window               wid;
static QMutex               mutex (true);
static bool                 window_created = true;
static bool                 wants_config;
static bool                 verbose;
static int                  running;
static int                  movie_width;
static int                  movie_height;
static int                  movie_length;
static int                  screen;
static const int            event_finished = QEvent::User;
static const int            event_playing = QEvent::User + 1;
static const int            event_size = QEvent::User + 2;
static QString              mrl;
static const char          *ao_driver;
static const char          *vo_driver;
GstElement * gst_elm_play, * videosink, * audiosink;
static QString elmentry ("entry");
static QString elmitem ("item");
static QString attname ("NAME");
static QString atttype ("TYPE");
static QString attdefault ("DEFAULT");
static QString attvalue ("VALUE");
static QString attstart ("START");
static QString attend ("END");
static QString valrange ("range");
static QString valnum ("num");
static QString valbool ("bool");
static QString valenum ("enum");
static QString valstring ("string");

extern "C" {
  // nothing yet
} // extern "C"


static void
cb_error (GstElement * play,
          GstElement * /*src*/,
          GError     *err,
          const char *debug,
          gpointer    /*data*/)
{
    fprintf (stderr, "cb_error: %s %s\n", err->message, debug);
    if (GST_STATE (play) == GST_STATE_PLAYING)
        gst_element_set_state (play, GST_STATE_READY);
    QApplication::postEvent (gstapp, new QEvent ((QEvent::Type)event_finished));
}

static void
cb_foundtag (GstElement * /*play*/,
             GstElement * /*src*/,
             const GstTagList * /*list*/,
             gpointer     /*data*/)
{
    fprintf (stderr, "cb_foundtag\n");
}

// NULL -> READY -> PAUSED -> PLAYING
static void
cb_eos (GstElement *play,
        gpointer   /*data*/)
{
    fprintf (stderr, "cb_eos\n");
    gst_element_set_state (play, GST_STATE_READY);
}

static void
cb_capsset (GstPad *pad,
            GParamSpec * /*pspec*/,
            gpointer /*data*/)
{
    const GstCaps *caps = GST_PAD_CAPS (pad);
    const GstStructure * s = gst_caps_get_structure (caps, 0);
    if (s) {
        const GValue *par;

        gst_structure_get_int (s, "width", &movie_width);
        gst_structure_get_int (s, "height", &movie_height);
        if ((par = gst_structure_get_value (s, "pixel-aspect-ratio"))) {
            int num = gst_value_get_fraction_numerator (par),
                den = gst_value_get_fraction_denominator (par);

            if (num > den)
                movie_width = (int) ((float) num * movie_width / den);
            else
                movie_height = (int) ((float) den * movie_height / num);
        }
        QApplication::postEvent (gstapp, new GstSizeEvent (movie_length, movie_width, movie_height));
    }
}


static void
cb_state (GstElement *play,
          GstElementState old_state,
          GstElementState new_state,
          gpointer    /*data*/)
{
    if (old_state >= GST_STATE_PLAYING && new_state <= GST_STATE_PAUSED) {
        QApplication::postEvent (gstapp, new QEvent ((QEvent::Type) event_finished));
    } else if (old_state <= GST_STATE_PAUSED && new_state >= GST_STATE_PLAYING) {
        QApplication::postEvent (gstapp, new QEvent ((QEvent::Type) event_playing));
    }
    if (old_state <= GST_STATE_READY && new_state >= GST_STATE_PAUSED) {
        const GList *list = NULL;

        mutex.lock ();
        g_object_get (G_OBJECT (gst_elm_play), "stream-info", &list, NULL);
        mutex.unlock ();
        for ( ; list != NULL; list = list->next) {
            GObject *info = (GObject *) list->data;
            gint type;
            GParamSpec *pspec;
            GEnumValue *val;
            GstPad *pad = NULL;

            g_object_get (info, "type", &type, NULL);
            pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (info), "type");
            val = g_enum_get_value (G_PARAM_SPEC_ENUM (pspec)->enum_class, type);

            if (strstr (val->value_name, "VIDEO")) {
                g_object_get (info, "object", &pad, NULL);
                pad = (GstPad *) GST_PAD_REALIZE (pad);
                if (GST_PAD_CAPS (pad)) {
                    cb_capsset (pad, NULL, NULL);
                } else {
                    g_signal_connect (pad, "notify::caps",
                            G_CALLBACK (cb_capsset), NULL);
                }
            }
        }
    }
}

GstSizeEvent::GstSizeEvent (int l, int w, int h)
  : QEvent ((QEvent::Type) event_size),
    length (l), width (w), height (h) 
{}

KMPlayerBackend::KMPlayerBackend ()
    : DCOPObject (QCString ("KMPlayerBackend")) {
}

KMPlayerBackend::~KMPlayerBackend () {}

void KMPlayerBackend::setURL (QString url) {
    mrl = url;
}

void KMPlayerBackend::setSubTitleURL (QString) {
}

void KMPlayerBackend::play () {
    gstapp->play ();
}

void KMPlayerBackend::stop () {
    QTimer::singleShot (0, gstapp, SLOT (stop ()));
}

void KMPlayerBackend::pause () {
    gstapp->pause ();
}

void KMPlayerBackend::seek (int, bool /*absolute*/) {
}

void KMPlayerBackend::hue (int h, bool) {
    gstapp->hue (10 * h);
}

void KMPlayerBackend::saturation (int s, bool) {
    gstapp->saturation (10 * s);
}

void KMPlayerBackend::contrast (int c, bool) {
    gstapp->contrast (10 * c);
}

void KMPlayerBackend::brightness (int b, bool) {
    gstapp->brightness (10 * b);
}

void KMPlayerBackend::volume (int v, bool) {
    gstapp->volume (v);
}

void KMPlayerBackend::frequency (int) {
}

void KMPlayerBackend::quit () {
    delete callback;
    callback = 0L;
    if (running)
        stop ();
    else
        QTimer::singleShot (0, qApp, SLOT (quit ()));
}

bool updateConfigEntry (const QString & name, const QString & value) {
    fprintf (stderr, "%s=%s\n", name.ascii (), (const char *) value.local8Bit ());
    return true;
}

void KMPlayerBackend::setConfig (QByteArray data) {
    QString err;
    int line, column;
    QDomDocument dom;
    if (dom.setContent (data, false, &err, &line, &column)) {
        if (dom.childNodes().length() == 1) {
            for (QDomNode node = dom.firstChild().firstChild();
                    !node.isNull ();
                    node = node.nextSibling ()) {
                QDomNamedNodeMap attr = node.attributes ();
                updateConfigEntry (attr.namedItem (attname).nodeValue (),
                                   attr.namedItem (attvalue).nodeValue ());
            }
        } else
            err = QString ("invalid data");
    }
    if (callback)
        callback->errorMessage (0, err);
}

KGStreamerPlayer::KGStreamerPlayer (int _argc, char ** _argv)
  : QApplication (_argc, _argv, false) {
}

void KGStreamerPlayer::init () {
    int xpos    = 0;
    int ypos    = 0;
    int width   = 320;
    int height  = 200;

    XLockDisplay(display);
    if (window_created)
        wid = XCreateSimpleWindow(display, XDefaultRootWindow(display),
                xpos, ypos, width, height, 1, 0, 0);
    if (!callback)
        QTimer::singleShot (10, this, SLOT (play ()));
    XSelectInput (display, wid,
                  (PointerMotionMask | ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask)); // | SubstructureNotifyMask));

    XUnlockDisplay(display);
    if (window_created) {
        fprintf (stderr, "map %lu\n", wid);
        XLockDisplay(display);
        XMapRaised(display, wid);
        XSync(display, False);
        XUnlockDisplay(display);
    }
}

KGStreamerPlayer::~KGStreamerPlayer () {
    if (window_created) {
        XLockDisplay (display);
        fprintf (stderr, "unmap %lu\n", wid);
        XUnmapWindow (display,  wid);
        XDestroyWindow(display,  wid);
        XSync (display, False);
        XUnlockDisplay (display);
    }
    gstapp = 0L;
}

void getConfigEntries (QByteArray & buf) {
    QDomDocument doc;
    QDomElement root = doc.createElement (QString ("document"));
    doc.appendChild (root);
    QCString exp = doc.toCString ();
    buf = exp;
    buf.resize (exp.length ()); // strip terminating \0
}

void KGStreamerPlayer::play () {
    if (gst_elm_play) {
        if (GST_STATE (gst_elm_play) == GST_STATE_PAUSED)
            gst_element_set_state (gst_elm_play, GST_STATE_PLAYING);
        return;
    }
    fprintf (stderr, "play %s\n", mrl.ascii ());
    if (mrl.isEmpty ())
        return;
    gchar *uri;
    mutex.lock ();
    gst_elm_play = gst_element_factory_make ("playbin", "player");
    if (!gst_elm_play) {
        fprintf (stderr, "couldn't create playbin\n");
        goto fail;
    }
    if (ao_driver && !strcmp (ao_driver, "alsa"))
        audiosink = gst_element_factory_make ("alsasink", "audiosink");
    else if (ao_driver && !strcmp (ao_driver, "arts"))
        audiosink = gst_element_factory_make ("artsdsink", "audiosink");
    else if (ao_driver && !strcmp (ao_driver, "esd"))
        audiosink = gst_element_factory_make ("esdsink", "audiosink");
    else
        audiosink = gst_element_factory_make ("osssink", "audiosink");
    if (!audiosink)
        goto fail;
    if (vo_driver && !strcmp (vo_driver, "xv"))
        videosink = gst_element_factory_make ("xvimagesink", "videosink");
    else
        videosink = gst_element_factory_make ("ximagesink", "videosink");
    if (!videosink)
        goto fail;
    g_object_set (G_OBJECT (gst_elm_play),
            "video-sink",  videosink,
            "audio-sink",  audiosink,
            NULL);
    g_signal_connect (gst_elm_play, "error", G_CALLBACK (cb_error), this);
    g_signal_connect (gst_elm_play, "found-tag", G_CALLBACK (cb_foundtag), this);
    g_signal_connect (gst_elm_play, "eos", G_CALLBACK (cb_eos), this);
    g_signal_connect (gst_elm_play, "state-change", G_CALLBACK (cb_state), this);
    gst_element_set_state (gst_elm_play, GST_STATE_READY);
    gst_x_overlay_set_xwindow_id (GST_X_OVERLAY (videosink), wid);
    gst_x_overlay_expose (GST_X_OVERLAY (videosink));
    if (GST_STATE (gst_elm_play) > GST_STATE_READY)
        gst_element_set_state (gst_elm_play, GST_STATE_READY);

    uri = g_strdup (mrl.ascii ());
    g_object_set (gst_elm_play, "uri", uri, NULL);
    gst_element_set_state (gst_elm_play, GST_STATE_PLAYING);
    mutex.unlock ();

    g_free (uri);
    return;
fail:
    QApplication::postEvent (gstapp, new QEvent ((QEvent::Type)event_finished));
}

void KGStreamerPlayer::pause () {
    mutex.lock ();
    if (GST_STATE (gst_elm_play) == GST_STATE_PLAYING)
        gst_element_set_state (gst_elm_play, GST_STATE_PAUSED);
    else
        gst_element_set_state (gst_elm_play, GST_STATE_PLAYING);
    mutex.unlock ();
}

void KGStreamerPlayer::stop () {
    fprintf (stderr, "stop %s\n", mrl.ascii ());
    if (gst_elm_play) {
        mutex.lock ();
        gst_element_set_state (gst_elm_play, GST_STATE_NULL);

        mutex.unlock ();
    } else
        QApplication::postEvent (gstapp, new QEvent ((QEvent::Type) event_finished));
}

void KGStreamerPlayer::finished () {
    QTimer::singleShot (10, this, SLOT (stop ()));
}

void KGStreamerPlayer::saturation (int val) {
}

void KGStreamerPlayer::hue (int val) {
}

void KGStreamerPlayer::contrast (int val) {
}

void KGStreamerPlayer::brightness (int val) {
}

void KGStreamerPlayer::volume (int) {
}

void KGStreamerPlayer::updatePosition () {
    QTimer::singleShot (500, this, SLOT (updatePosition ()));
}

bool KGStreamerPlayer::event (QEvent * e) {
    switch (e->type()) {
        case event_finished: {
            fprintf (stderr, "event_finished\n");
            if (gst_elm_play) {
                mutex.lock ();
                gst_object_unref (GST_OBJECT (gst_elm_play));
                if (audiosink)
                    gst_object_unref (GST_OBJECT (audiosink));
                if (videosink) {
                    if (videosink && GST_IS_X_OVERLAY (videosink))
                        gst_x_overlay_set_xwindow_id (GST_X_OVERLAY (videosink), 0);
                    gst_object_unref (GST_OBJECT (videosink));
                }
                mutex.unlock ();
                gst_elm_play = 0L;
                audiosink = 0L;
                videosink = 0L;
            }
            if (callback)
                callback->finished ();
            else
                QTimer::singleShot (0, this, SLOT (quit ()));
            break;
        }
        case event_playing:
            if (callback) {
                callback->playing ();
                QTimer::singleShot (500, this, SLOT (updatePosition ()));
            }
            break;
                //callback->movieParams (se->length/100, se->width, se->height, se->height ? 1.0*se->width/se->height : 1.0);
        case event_size: {
            GstSizeEvent * se = static_cast <GstSizeEvent *> (e);                
            if (callback) {
                if (se->length < 0) se->length = 0;
                callback->movieParams (se->length/100, se->width, se->height, se->height ? 1.0*se->width/se->height : 1.0);
            } 
            if (window_created && movie_width > 0 && movie_height > 0) {
                XLockDisplay (display);
                XResizeWindow (display, wid, movie_width, movie_height);
                XFlush (display);
                XUnlockDisplay (display);
            }
            break;
        }
        default:
            return false;
    }
    return true;
}
 
class XEventThread : public QThread {
protected:
    void run () {
        while (true) {
            XEvent   xevent;
            XNextEvent(display, &xevent);
            switch(xevent.type) {
                case ClientMessage:
                    if (xevent.xclient.format == 8 &&
                            !strncmp(xevent.xclient.data.b, "quit_now", 8)) {
                        fprintf(stderr, "request quit\n");
                        return;
                    }
                    break;
                case KeyPress: {
                    XKeyEvent  kevent;
                    KeySym     ksym;
                    char       kbuf[256];
                    int        len;
                    kevent = xevent.xkey;
                    XLockDisplay(display);
                    len = XLookupString(&kevent, kbuf, sizeof(kbuf), &ksym, NULL);
                    XUnlockDisplay(display);
                    fprintf(stderr, "keypressed 0x%x 0x%x\n", kevent.keycode, ksym);
                    switch (ksym) {
                        case XK_q:
                        case XK_Q:
                            gstapp->lock ();
                            gstapp->stop ();
                            gstapp->unlock ();
                            break;
                    }
                    break;
                }
                case Expose:
                    if(xevent.xexpose.count != 0 || xevent.xexpose.window != wid)
                        break;
                    mutex.lock ();
                    if (videosink && GST_IS_X_OVERLAY (videosink)) {
                        gst_x_overlay_set_xwindow_id (GST_X_OVERLAY (videosink), wid);
                        gst_x_overlay_expose (GST_X_OVERLAY (videosink));
                    }
                    mutex.unlock ();
                    break;

                case ConfigureNotify:
                    break;
                default:
                    ; //if (xevent.type < LASTEvent)
                      //  fprintf (stderr, "event %d\n", xevent.type);
            }
        }
    }
};

int main(int argc, char **argv) {
    if (!XInitThreads ()) {
        fprintf (stderr, "XInitThreads () failed\n");
        return 1;
    }
    display = XOpenDisplay(NULL);
    screen  = XDefaultScreen(display);

    gst_init (NULL, NULL);

    gstapp = new KGStreamerPlayer (argc, argv);

    for(int i = 1; i < argc; i++) {
        if (!strcmp (argv [i], "-ao")) {
            ao_driver = argv [++i];
        } else if (!strcmp (argv [i], "-vo")) {
            vo_driver = argv [++i];
        } else if (!strcmp (argv [i], "-wid") || !strcmp (argv [i], "-window-id")) {
            wid = atol (argv [++i]);
            window_created = false;
        } else if (!strcmp (argv [i], "-root")) {
            wid =  XDefaultRootWindow (display);
            window_created = false;
        } else if (!strcmp (argv [i], "-window")) {
            ;
        } else if (!strcmp (argv [i], "-v")) {
            verbose = true;
        } else if (!strcmp (argv [i], "-c")) {
            wants_config = true;
        } else if (!strcmp (argv [i], "-f") && i < argc - 1) {
            strncpy (configfile, argv [++i], sizeof (configfile));
            configfile[sizeof (configfile) - 1] = 0;
        } else if (!strcmp (argv [i], "-cb")) {
            QString str = argv [++i];
            int pos = str.find ('/');
            if (pos > -1) {
                fprintf (stderr, "callback is %s %s\n", str.left (pos).ascii (), str.mid (pos + 1).ascii ());
                callback = new KMPlayerCallback_stub 
                    (str.left (pos).ascii (), str.mid (pos + 1).ascii ());
            }
        } else if (!strncmp (argv [i], "-", 1)) {
            fprintf (stderr, "usage: %s [-ao <audio driver>] [-f <config file>] [-v] [(-wid|-window-id) <window>] [(-root|-window)] [-cb <DCOP callback name> [-c]] [<url>]\n", argv[0]);
            delete gstapp;
            return 1;
        } else {
            mrl = QString::fromLocal8Bit (argv[i]);
        }
    }

    DCOPClient dcopclient;
    dcopclient.registerAs ("kgstreamerplayer");
    KMPlayerBackend player;

    XEventThread * eventThread = new XEventThread;
    eventThread->start ();

    gstapp->init ();

    if (callback) {
        QByteArray buf;
        if (wants_config)
            getConfigEntries (buf);
        callback->started (dcopclient.appId (), buf);
    }
    gstapp->exec ();

    XLockDisplay(display);
    XClientMessageEvent ev = {
        ClientMessage, 0, true, display, wid, 
        XInternAtom (display, "XVIDEO", false), 8, {b: "quit_now"}
    };
    XSendEvent (display, wid, FALSE, StructureNotifyMask, (XEvent *) & ev);
    XFlush (display);
    XUnlockDisplay(display);
    eventThread->wait (500);
    delete eventThread;

    gstapp->stop ();
    delete gstapp;

    fprintf (stderr, "closing display\n");
    XCloseDisplay (display);
    fprintf (stderr, "done\n");
    return 0;
}

#include "gstplayer.moc"