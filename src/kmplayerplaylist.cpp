/**
 * Copyright (C) 2004 by Koos Vriezen <koos ! vriezen ? xs4all ! nl>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License version 2 as published by the Free Software Foundation.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA 02111-1307, USA.
 **/

#include <qdom.h>
#include <qxml.h>
#include <qfile.h>
#include <qregexp.h>
#include <qtextstream.h>
#include <kdebug.h>
#include "kmplayerplaylist.h"

static Element * fromXMLDocumentGroup (ElementPtr d, const QString & tag) {
    const char * const name = tag.latin1 ();
    if (!strcmp (name, "smil"))
        return new Smil (d);
    else if (!strcasecmp (name, "asx"))
        return new Asx (d);
    return 0L;
}

static Element * fromScheduleGroup (ElementPtr d, const QString & tag) {
    if (!strcmp (tag.latin1 (), "par"))
        return new Par (d);
    else if (!strcmp (tag.latin1 (), "seq"))
        return new Seq (d);
    // else if (!strcmp (tag.latin1 (), "excl"))
    //    return new Seq (d, p);
    return 0L;
}

static Element * fromMediaContentGroup (ElementPtr d, const QString & tag) {
    if (!strcmp (tag.latin1 (), "video") || !strcmp (tag.latin1 (), "audio"))
        return new MediaType (d, tag);
    // text, img, animation, textstream, ref, brush
    return 0L;
};

static Element * fromContentControlGroup (ElementPtr d, const QString & tag) {
    if (!strcmp (tag.latin1 (), "switch"))
        return new Switch (d);
    return 0L;
}

//-----------------------------------------------------------------------------

KDE_NO_CDTOR_EXPORT Element::~Element () {
    clear ();
}

KDE_NO_EXPORT Document * Element::document () {
    return dynamic_cast<Document*>(static_cast<Element *>(m_doc));
}

KDE_NO_EXPORT Mrl * Element::mrl () {
    return dynamic_cast<Mrl*>(this);
}

KDE_NO_EXPORT const Mrl * Element::mrl () const {
    return dynamic_cast<const Mrl*>(this);
}

KDE_NO_EXPORT const char * Element::tagName () const {
    return "element";
}

KDE_NO_EXPORT void Element::clear () {
    while (m_first_child != m_last_child) {
        // avoid stack abuse with 10k children derefing each other
        if (m_last_child->m_parent)
        m_last_child->m_parent = 0L;
        m_last_child = m_last_child->m_prev;
        m_last_child->m_next = 0L;
    }
    if (m_first_child)
        m_first_child->m_parent = 0L;
    m_first_child = m_last_child = 0L;
}

KDE_NO_EXPORT void Element::appendChild (ElementPtr c) {
    if (!m_first_child) {
        m_first_child = m_last_child = c;
    } else {
        m_last_child->m_next = c;
        c->m_prev = m_last_child;
        m_last_child = c;
    }
    c->m_parent = m_self;
}

KDE_NO_EXPORT void Element::insertBefore (ElementPtr c, ElementPtr b) {
    if (b->m_prev) {
        b->m_prev->m_next = c;
        c->m_prev = b->m_prev;
    } else {
        c->m_prev = 0L;
        m_first_child = c;
    }
    b->m_prev = c;
    c->m_next = b;
    c->m_parent = m_self;
}

KDE_NO_EXPORT void Element::removeChild (ElementPtr c) {
    if (c->m_prev) {
        c->m_prev->m_next = c->m_next;
        c->m_prev = 0L;
    } else
        m_first_child = c->m_next;
    if (c->m_next) {
        c->m_next->m_prev = c->m_prev;
        c->m_next = 0L;
    } else
        m_last_child = c->m_prev;
    c->m_parent = 0L;
}

KDE_NO_EXPORT void Element::replaceChild (ElementPtr _new, ElementPtr old) {
    if (old->m_prev) {
        old->m_prev->m_next = _new;
        _new->m_prev = old->m_prev;
        old->m_prev = 0L;
    } else {
        _new->m_prev = 0L;
        m_first_child = _new;
    }
    if (old->m_next) {
        old->m_next->m_prev = _new;
        _new->m_next = old->m_next;
        old->m_next = 0L;
    } else {
        _new->m_next = 0L;
        m_last_child = _new;
    }
    _new->m_parent = m_self;
    old->m_parent = 0L;
}

KDE_NO_EXPORT ElementPtr Element::childFromTag (ElementPtr, const QString &) {
    return 0L;
}

KDE_NO_EXPORT void Element::setAttributes (const QXmlAttributes & atts) {
    for (int i = 0; i < atts.length (); i++)
        kdDebug () << " " << atts.qName (i) << "=" << atts.value (i) << endl;
}

bool Element::isMrl () {
    return false;
}

KDE_NO_CDTOR_EXPORT Mrl::~Mrl () {}

bool Mrl::isMrl () {
    return !src.isEmpty ();
}

//-----------------------------------------------------------------------------

KDE_NO_CDTOR_EXPORT Document::Document (const QString & s) {
    m_doc = this;
    m_self = m_doc;
    src = s;
}

KDE_NO_CDTOR_EXPORT Document::~Document () {
    kdDebug () << "~Document\n";
};

KDE_NO_EXPORT ElementPtr Document::childFromTag (ElementPtr d, const QString & tag) {
    Element * elm = fromXMLDocumentGroup (d, tag);
    if (elm)
        return elm->self ();
    return 0L;
}

KDE_NO_EXPORT void Document::dispose () {
    clear ();
    m_doc = 0L;
}

bool Document::isMrl () {
    return !hasChildNodes ();
}

//-----------------------------------------------------------------------------

KDE_NO_EXPORT ElementPtr Smil::childFromTag (ElementPtr d, const QString & tag) {
    if (!strcmp (tag.latin1 (), "body"))
        return (new Body (d))->self ();
    // else if head
    return 0L;
}

//-----------------------------------------------------------------------------

KDE_NO_EXPORT ElementPtr Body::childFromTag (ElementPtr d, const QString & tag) {
    Element * elm = fromScheduleGroup (d, tag);
    if (!elm) elm = fromMediaContentGroup (d, tag);
    if (!elm) elm = fromContentControlGroup (d, tag);
    if (elm)
        return elm->self ();
    return 0L;
}

//-----------------------------------------------------------------------------

KDE_NO_EXPORT ElementPtr Par::childFromTag (ElementPtr d, const QString & tag) {
    Element * elm = fromScheduleGroup (d, tag);
    if (!elm) elm = fromMediaContentGroup (d, tag);
    if (!elm) elm = fromContentControlGroup (d, tag);
    if (elm)
        return elm->self ();
    return 0L;
}

//-----------------------------------------------------------------------------

KDE_NO_EXPORT ElementPtr Seq::childFromTag (ElementPtr d, const QString & tag) {
    Element * elm = fromScheduleGroup (d, tag);
    if (!elm) elm = fromMediaContentGroup (d, tag);
    if (!elm) elm = fromContentControlGroup (d, tag);
    if (elm)
        return elm->self ();
    return 0L;
}

//-----------------------------------------------------------------------------

KDE_NO_EXPORT ElementPtr Switch::childFromTag (ElementPtr d, const QString & tag) {
    Element * elm = fromContentControlGroup (d, tag);
    if (!elm) elm = fromMediaContentGroup (d, tag);
    if (elm)
        return elm->self ();
    return 0L;
}

bool Switch::isMrl () {
    return false; // TODO eval conditions on children and choose one
}

//-----------------------------------------------------------------------------

KDE_NO_CDTOR_EXPORT MediaType::MediaType (ElementPtr d, const QString & t)
    : Mrl (d), m_type (t), bitrate (0) {}

KDE_NO_EXPORT ElementPtr MediaType::childFromTag (ElementPtr d, const QString & tag) {
    Element * elm = fromContentControlGroup (d, tag);
    if (elm)
        return elm->self ();
    return 0L;
}

KDE_NO_EXPORT void MediaType::setAttributes (const QXmlAttributes & atts) {
    for (int i = 0; i < atts.length (); i++) {
        const char * attr = atts.qName (i).latin1();
        if (!strcmp (attr, "system-bitrate"))
            bitrate = atts.value (i).toInt ();
        else if (!strcmp (attr, "src"))
            src = atts.value (i);
        else if (!strcmp (attr, "type"))
            mimetype = atts.value (i);
        else
            kdError () << "Warning: unhandled MediaType attr: " << attr << "=" << atts.value (i) << endl;
    }
    kdDebug () << "MediaType attr found bitrate: " << bitrate << " src: " << (src.isEmpty() ? "-" : src) << " type: " << (mimetype.isEmpty() ? "-" : mimetype) << endl;
}

//-----------------------------------------------------------------------------

KDE_NO_EXPORT ElementPtr Asx::childFromTag (ElementPtr d, const QString & tag) {
    const char * name = tag.latin1 ();
    if (!strcasecmp (name, "entry"))
        return (new Entry (d))->self ();
    else if (!strcasecmp (name, "entryref"))
        return (new EntryRef (d))->self ();
    return 0L;
}

//-----------------------------------------------------------------------------

KDE_NO_EXPORT ElementPtr Entry::childFromTag (ElementPtr d, const QString & tag) {
    const char * name = tag.latin1 ();
    if (!strcasecmp (name, "ref"))
        return (new Ref (d))->self ();
    return 0L;
}

//-----------------------------------------------------------------------------

KDE_NO_EXPORT void Ref::setAttributes (const QXmlAttributes & atts) {
    for (int i = 0; i < atts.length (); i++)
        if (!strcasecmp (atts.qName (i).latin1(), "href"))
            src = atts.value (i);
        else
            kdError () << "Warning: unhandled Ref attr: " << atts.qName (i) << "=" << atts.value (i) << endl;
    kdDebug () << "Ref attr found src: " << src << endl;
}

//-----------------------------------------------------------------------------

KDE_NO_EXPORT void EntryRef::setAttributes (const QXmlAttributes & atts) {
    for (int i = 0; i < atts.length (); i++)
        if (!strcasecmp (atts.qName (i).latin1(), "href"))
            src = atts.value (i);
        else
            kdError () << "unhandled EntryRef attr: " << atts.qName (i) << "=" << atts.value (i) << endl;
    kdDebug () << "EntryRef attr found src: " << src << endl;
}

//-----------------------------------------------------------------------------

KDE_NO_CDTOR_EXPORT GenericURL::GenericURL (ElementPtr d, const QString & s)
 : Mrl (d) {
    src = s;
}

KDE_NO_EXPORT ElementPtr GenericURL::childFromTag (ElementPtr d, const QString & tag) {
    Element * elm = fromXMLDocumentGroup (d, tag);
    if (elm)
        return elm->self ();
    return 0L;
}

bool GenericURL::isMrl () {
    return !hasChildNodes ();
}

//-----------------------------------------------------------------------------

