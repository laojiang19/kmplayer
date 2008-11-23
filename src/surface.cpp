/**
  This file belong to the KMPlayer project, a movie player plugin for Konqueror
  Copyright (C) 2008  Koos Vriezen <koos.vriezen@gmail.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**/

#include "config-kmplayer.h"

#ifdef KMPLAYER_WITH_CAIRO
# include <cairo.h>
#endif

#include <qwidget.h>

#include <kdebug.h>

#include "surface.h"
#include "viewarea.h"

using namespace KMPlayer;


KDE_NO_CDTOR_EXPORT Surface::Surface (ViewArea *widget)
  : bounds (SRect (0, 0, widget->width (), widget->height ())),
    xscale (1.0), yscale (1.0),
    background_color (widget->palette().color (widget->backgroundRole()).rgb()),
#ifdef KMPLAYER_WITH_CAIRO
    surface (0L),
#endif
    dirty (false),
    scroll (false),
    view_widget (widget) {
    background_color = widget->palette().color (widget->backgroundRole()).rgb();
}

Surface::~Surface() {
#ifdef KMPLAYER_WITH_CAIRO
    if (surface)
        cairo_surface_destroy (surface);
#endif
}

void Surface::clear () {
    m_first_child = 0L;
    background_color = view_widget->palette().color (view_widget->backgroundRole()).rgb();
}

void Surface::remove () {
    Surface *sp = parentNode ().ptr ();
    if (sp) {
        sp->markDirty ();
        sp->removeChild (this);
    }
}

void Surface::resize (const SRect &rect, bool parent_resized) {
    SRect old_bounds = bounds;
    bounds = rect;
    if (parent_resized || old_bounds != rect) {

        if (parent_resized || old_bounds.size != rect.size) {
            virtual_size = SSize (); //FIXME try to preserve scroll on resize
            markDirty ();
#ifdef KMPLAYER_WITH_CAIRO
            if (surface) {
                cairo_surface_destroy (surface);
                surface = NULL;
            }
#endif
            updateChildren (true);
        } else if (parentNode ()) {
            parentNode ()->markDirty ();
        }
        if (parentNode ())
            parentNode ()->repaint (old_bounds.unite (rect));
        else
            repaint ();
    }
}

void Surface::markDirty () {
    for (Surface *s = this; s && !s->dirty; s = s->parentNode ().ptr ())
        s->dirty = true;
}

void Surface::updateChildren (bool parent_resized) {
    for (SurfacePtr c = firstChild (); c; c = c->nextSibling ())
        if (c->node)
            c->node->message (MsgSurfaceBoundsUpdate, (void *) parent_resized);
        else
            kError () << "Surface without node";
}

Surface *Surface::createSurface (NodePtr owner, const SRect & rect) {
    Surface *surface = new Surface (view_widget);
    surface->node = owner;
    surface->bounds = rect;
    appendChild (surface);
    return surface;
}

KDE_NO_EXPORT IRect Surface::toScreen (Single x, Single y, Single w, Single h) {
    //FIXME: handle scroll
    Matrix matrix (0, 0, xscale, yscale);
    matrix.translate (bounds.x (), bounds.y ());
    for (SurfacePtr s = parentNode(); s; s = s->parentNode()) {
        matrix.transform(Matrix (0, 0, s->xscale, s->yscale));
        matrix.translate (s->bounds.x (), s->bounds.y ());
    }
    matrix.getXYWH (x, y, w, h);
    return IRect (x, y, w, h);
}

static void clipToScreen (Surface *s, Matrix &m, IRect &clip) {
    Surface *ps = s->parentNode ().ptr ();
    if (!ps) {
        clip = IRect (s->bounds);
        m = Matrix (s->bounds.x (), s->bounds.y (), s->xscale, s->yscale);
    } else {
        clipToScreen (ps, m, clip);
        SRect r = s->bounds;
        Single x = r.x (), y = r.y (), w = r.width (), h = r.height ();
        m.getXYWH (x, y, w, h);
        clip = clip.intersect (IRect (x, y, w, h));
        Matrix m1 = m;
        m = Matrix (s->bounds.x (), s->bounds.y (), s->xscale, s->yscale);
        m.transform (m1);
        if (!s->virtual_size.isEmpty ())
            m.translate (-s->x_scroll, -s->y_scroll);
    }
}


KDE_NO_EXPORT void Surface::repaint (const SRect &r) {
    Matrix matrix;
    IRect clip;
    clipToScreen (this, matrix, clip);
    Single x = r.x (), y = r.y (), w = r.width (), h = r.height ();
    matrix.getXYWH (x, y, w, h);
    clip = clip.intersect (IRect (x, y, w, h));
    if (!clip.isEmpty ())
        view_widget->scheduleRepaint (clip);
}

KDE_NO_EXPORT void Surface::repaint () {
    Surface *ps = parentNode ().ptr ();
    if (ps)
        ps->repaint (bounds);
    else
        view_widget->scheduleRepaint (IRect (bounds));
}
