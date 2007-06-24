/*
* Copyright (C) 2007  Koos Vriezen <koos.vriezen@gmail.com>
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/* gcc -o knpplayer `pkg-config --libs --cflags gtk+-x11-2.0` `pkg-config --libs --cflags dbus-glib-1` `nspr-config --libs --cflags` npplayer.c

http://devedge-temp.mozilla.org/library/manuals/2002/plugin/1.0/
http://dbus.freedesktop.org/doc/dbus/libdbus-tutorial.html
*/

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>

#include <glib/gprintf.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#define XP_UNIX
#define MOZ_X11
#include "moz-sdk/npupp.h"

typedef const char* (* NP_LOADDS NP_GetMIMEDescriptionUPP)();
typedef NPError (* NP_InitializeUPP)(NPNetscapeFuncs*, NPPluginFuncs*);
typedef NPError (* NP_ShutdownUPP)(void);

static gchar *plugin;
static gchar *object_url;
static gchar *mimetype;

static DBusConnection *dbus_connection;
static char *service_name;
static gchar *callback_service;
static gchar *callback_path;
static GModule *library;
static GtkWidget *xembed;
static Window socket_id;
static Window parent_id;
static int stdin_read_watch;

static NPPluginFuncs np_funcs;       /* plugin functions              */
static NPP npp;                      /* single instance of the plugin */
static NPWindow np_window;
static NPObject *scriptable_peer;
static NPSavedData *saved_data;
static NPClass js_class;
static GSList *stream_list;
static GTree *identifiers;
static int js_obj_counter;
static char stream_buf[4096];
typedef struct _StreamInfo {
    NPStream np_stream;
    void *notify_data;
    unsigned int stream_buf_pos;
    unsigned int stream_pos;
    unsigned int total;
    unsigned int reason;
    char *url;
    char *mimetype;
    char *target;
    char notify;
} StreamInfo;
struct JsObject;
typedef struct _JsObject {
    NPObject npobject;
    struct _JsObject * parent;
    char * name;
} JsObject;

static NP_GetMIMEDescriptionUPP npGetMIMEDescription;
static NP_InitializeUPP npInitialize;
static NP_ShutdownUPP npShutdown;

static StreamInfo *addStream (const char *url, const char *mime, const char *target, void *notify_data, int notify);
static gboolean requestStream (void * si);
static gboolean destroyStream (void * si);
static void callFunction (const char *func, int first_arg_type, ...);
static char * evaluate (const char *script);

/*----------------%<---------------------------------------------------------*/

static void print (const char * format, ...) {
    va_list vl;
    va_start (vl, format);
    vprintf (format, vl);
    va_end (vl);
    fflush (stdout);
}

/*----------------%<---------------------------------------------------------*/

static void freeStream (StreamInfo *si) {
    stream_list = g_slist_remove (stream_list, si);
    g_free (si->url);
    if (si->mimetype)
        g_free (si->mimetype);
    if (si->target)
        g_free (si->target);
    free (si);
}

/*----------------%<---------------------------------------------------------*/

static void createJsName (JsObject * obj, char **name, uint32_t * len) {
    int slen = strlen (obj->name);
    if (obj->parent) {
        *len += slen + 1;
        createJsName (obj->parent, name, len);
    } else {
        *name = (char *) malloc (*len + slen + 1);
        *(*name + *len + slen) = 0;
        *len = 0;
    }
    if (obj->parent) {
        *(*name + *len) = '.';
        *len += 1;
    }
    memcpy (*name + *len, obj->name, slen);
    *len += slen;
}

/*----------------%<---------------------------------------------------------*/

static NPObject * nsCreateObject (NPP instance, NPClass *aClass) {
    NPObject *obj;
    if (aClass && aClass->allocate)
        obj = aClass->allocate (instance, aClass);
    else
        obj = js_class.allocate (instance, &js_class);/*add null class*/
    print ("NPN_CreateObject\n");
    obj->referenceCount = 1;
    return obj;
}

static NPObject *nsRetainObject (NPObject *npobj) {
    print( "nsRetainObject %p\n", npobj);
    npobj->referenceCount++;
    return npobj;
}

static void nsReleaseObject (NPObject *obj) {
    print ("NPN_ReleaseObject\n");
    if (! (--obj->referenceCount))
        obj->_class->deallocate (obj);
}

static NPError nsGetURL (NPP instance, const char* url, const char* target) {
    (void)instance;
    print ("nsGetURL %s %s\n", url, target);
    addStream (url, 0L, target, 0L, 1);
    return NPERR_NO_ERROR;
}

static NPError nsPostURL (NPP instance, const char *url,
        const char *target, uint32 len, const char *buf, NPBool file) {
    (void)instance; (void)len; (void)buf; (void)file;
    print ("nsPostURL %s %s\n", url, target);
    addStream (url, 0L, target, 0L, 1);
    return NPERR_NO_ERROR;
}

static NPError nsRequestRead (NPStream *stream, NPByteRange *rangeList) {
    (void)stream; (void)rangeList;
    print ("nsRequestRead\n");
    return NPERR_NO_ERROR;
}

static NPError nsNewStream (NPP instance, NPMIMEType type,
        const char *target, NPStream **stream) {
    (void)instance; (void)type; (void)stream; (void)target;
    print ("nsNewStream\n");
    return NPERR_NO_ERROR;
}

static int32 nsWrite (NPP instance, NPStream* stream, int32 len, void *buf) {
    (void)instance; (void)len; (void)buf; (void)stream;
    print ("nsWrite\n");
    return 0;
}

static NPError nsDestroyStream (NPP instance, NPStream *stream, NPError reason) {
    (void)instance; (void)stream; (void)reason;
    print ("nsDestroyStream\n");
    g_timeout_add (0, destroyStream, g_slist_nth_data (stream_list, 0));
    return NPERR_NO_ERROR;
}

static void nsStatus (NPP instance, const char* message) {
    (void)instance;
    print ("NPN_Status %s\n", message);
}

static const char* nsUserAgent (NPP instance) {
    (void)instance;
    print ("NPN_UserAgent\n");
    return "";
}

static void *nsAlloc (uint32 size) {
    return malloc (size);
}

static void nsMemFree (void* ptr) {
    free (ptr);
}

static uint32 nsMemFlush (uint32 size) {
    (void)size;
    print ("NPN_MemFlush\n");
    return 0;
}

static void nsReloadPlugins (NPBool reloadPages) {
    (void)reloadPages;
    print ("NPN_ReloadPlugins\n");
}

static JRIEnv* nsGetJavaEnv () {
    print ("NPN_GetJavaEnv\n");
    return NULL;
}

static jref nsGetJavaPeer (NPP instance) {
    (void)instance;
    print ("NPN_GetJavaPeer\n");
    return NULL;
}

static NPError nsGetURLNotify (NPP instance, const char* url, const char* target, void *notify) {
    (void)instance;
    print ("NPN_GetURLNotify %s %s\n", url, target);
    addStream (url, 0L, target, notify, 1);
    return NPERR_NO_ERROR;
}

static NPError nsPostURLNotify (NPP instance, const char* url, const char* target, uint32 len, const char* buf, NPBool file, void *notify) {
    (void)instance; (void)len; (void)buf; (void)file;
    print ("NPN_PostURLNotify\n");
    addStream (url, 0L, target, notify, 1);
    return NPERR_NO_ERROR;
}

static NPError nsGetValue (NPP instance, NPNVariable variable, void *value) {
    print ("NPN_GetValue %d\n", variable & ~NP_ABI_MASK);
    switch (variable) {
        case NPNVxDisplay:
            *(void**)value = (void*)(long) gdk_x11_get_default_xdisplay ();
            break;
        case NPNVxtAppContext:
            *(void**)value = NULL;
            break;
        case NPNVnetscapeWindow:
            print ("NPNVnetscapeWindow\n");
            break;
        case NPNVjavascriptEnabledBool:
            *(int*)value = 1;
            break;
        case NPNVasdEnabledBool:
            *(int*)value = 0;
            break;
        case NPNVisOfflineBool:
            *(int*)value = 0;
            break;
        case NPNVserviceManager:
            *(int*)value = 0;
            break;
        case NPNVToolkit:
            *(int*)value = NPNVGtk2;
            break;
        case NPNVSupportsXEmbedBool:
            *(int*)value = 1;
            break;
        case NPNVWindowNPObject: {
            JsObject * obj = (JsObject *) nsCreateObject (instance, &js_class);
            obj->name = g_strdup ("window");
            *(NPObject**)value = (NPObject *) obj;
            break;
        }
        case NPNVPluginElementNPObject: {
            JsObject * obj = (JsObject *) nsCreateObject (instance, &js_class);
            obj->name = g_strdup ("this");
            *(NPObject**)value = (NPObject *) obj;
            break;
        }
        default:
            *(int*)value = 0;
            print ("unknown value\n");
    }
    return NPERR_NO_ERROR;
}

static NPError nsSetValue (NPP instance, NPPVariable variable, void *value) {
    /* NPPVpluginWindowBool */
    (void)instance; (void)value;
    print ("NPN_SetValue %d\n", variable & ~NP_ABI_MASK);
    return NPERR_NO_ERROR;
}

static void nsInvalidateRect (NPP instance, NPRect *invalidRect) {
    (void)instance; (void)invalidRect;
    print ("NPN_InvalidateRect\n");
}

static void nsInvalidateRegion (NPP instance, NPRegion invalidRegion) {
    (void)instance; (void)invalidRegion;
    print ("NPN_InvalidateRegion\n");
}

static void nsForceRedraw (NPP instance) {
    (void)instance;
    print ("NPN_ForceRedraw\n");
}

static NPIdentifier nsGetStringIdentifier (const NPUTF8* name) {
    print ("NPN_GetStringIdentifier %s\n", name);
    gpointer id = g_tree_lookup (identifiers, name);
    if (!id) {
        id = strdup (name);
        g_tree_insert (identifiers, id, id);
    }
    return id;
}

static void nsGetStringIdentifiers (const NPUTF8** names, int32_t nameCount,
        NPIdentifier* ids) {
    (void)names; (void)nameCount; (void)ids;
    print ("NPN_GetStringIdentifiers\n");
}

static NPIdentifier nsGetIntIdentifier (int32_t intid) {
    print ("NPN_GetIntIdentifier %d\n", intid);
    return NULL;
}

static bool nsIdentifierIsString (NPIdentifier name) {
    print ("NPN_IdentifierIsString\n");
    return !!g_tree_lookup (identifiers, name);
}

static NPUTF8 * nsUTF8FromIdentifier (NPIdentifier name) {
    print ("NPN_UTF8FromIdentifier\n");
    return g_tree_lookup (identifiers, name);
}

static int32_t nsIntFromIdentifier (NPIdentifier identifier) {
    (void)identifier;
    print ("NPN_IntFromIdentifier\n");
    return 0;
}

static bool nsInvoke (NPP instance, NPObject * npobj, NPIdentifier method,
        const NPVariant *args, uint32_t arg_count, NPVariant *result) {
    (void)instance;
    /*print ("NPN_Invoke %s\n", id);*/
    return npobj->_class->invoke (npobj, method, args, arg_count, result);
}

static bool nsInvokeDefault (NPP instance, NPObject * npobj,
        const NPVariant * args, uint32_t arg_count, NPVariant * result) {
    (void)instance;
    return npobj->_class->invokeDefault (npobj,args, arg_count, result);
}

static bool nsEvaluate (NPP instance, NPObject * npobj, NPString * script,
        NPVariant * result) {
    char * this_var;
    char * this_var_type;
    char * this_var_string;
    char * jsscript;
    (void) npobj; /*FIXME scope, search npobj window*/
    print ("NPN_Evaluate\n");

    /* assign to a js variable */
    this_var = (char *) malloc (64);
    sprintf (this_var, "this.__kmplayer__obj_%d", js_obj_counter);

    jsscript = (char *) malloc (strlen (this_var) + script->utf8length + 3);
    sprintf (jsscript, "%s=%s;", this_var, script->utf8characters);
    this_var_string = evaluate (jsscript);
    free (jsscript);

    if (this_var_string) {
        /* get type of js this_var */
        jsscript = (char *) malloc (strlen (this_var) + 9);
        sprintf (jsscript, "typeof %s;", this_var);
        this_var_type = evaluate (jsscript);
        free (jsscript);

        if (this_var_type) {
            if (!strcasecmp (this_var_type, "undefined")) {
                result->type = NPVariantType_Null;
            } else if (!strcasecmp (this_var_type, "object")) {
                JsObject *jo = (JsObject *)nsCreateObject (instance, &js_class);
                js_obj_counter++;
                result->type = NPVariantType_Object;
                jo->name = g_strdup (this_var);
                result->value.objectValue = (NPObject *)jo;
            } else { /* FIXME numbers/void/undefined*/
                result->type = NPVariantType_String;
                result->value.stringValue.utf8characters =
                    g_strdup (this_var_string);
                result->value.stringValue.utf8length = strlen (this_var_string);
            }
            g_free (this_var_type);
        }
        g_free (this_var_string);
    } else {
        print ("   => error\n");
        return false;
    }
    free (this_var);

    return true;
}

static bool nsGetProperty (NPP instance, NPObject * npobj,
        NPIdentifier property, NPVariant * result) {
    (void)instance;
    return npobj->_class->getProperty (npobj, property, result);
}

static bool nsSetProperty (NPP instance, NPObject * npobj,
        NPIdentifier property, const NPVariant *value) {
    (void)instance;
    return npobj->_class->setProperty (npobj, property, value);
}

static bool nsRemoveProperty (NPP inst, NPObject * npobj, NPIdentifier prop) {
    (void)inst;
    return npobj->_class->removeProperty (npobj, prop);
}

static bool nsHasProperty (NPP instance, NPObject * npobj, NPIdentifier prop) {
    (void)instance;
    return npobj->_class->hasProperty (npobj, prop);
}

static bool nsHasMethod (NPP instance, NPObject * npobj, NPIdentifier method) {
    (void)instance;
    return npobj->_class->hasMethod (npobj, method);
}

static void nsReleaseVariantValue (NPVariant * variant) {
    print ("NPN_ReleaseVariantValue\n");
    switch (variant->type) {
        case NPVariantType_String:
            if (variant->value.stringValue.utf8characters)
                g_free ((char *) variant->value.stringValue.utf8characters);
            break;
        case NPVariantType_Object:
            if (variant->value.objectValue)
                nsReleaseObject (variant->value.objectValue);
            break;
        default:
            break;
    }
    variant->type = NPVariantType_Null;
}

static void nsSetException (NPObject *npobj, const NPUTF8 *message) {
    (void)npobj;
    print ("NPN_SetException %s\n", message);
}

static bool nsPushPopupsEnabledState (NPP instance, NPBool enabled) {
    (void)instance;
    print ("NPN_PushPopupsEnabledState %d\n", enabled);
    return false;
}

static bool nsPopPopupsEnabledState (NPP instance) {
    (void)instance;
    print ("NPN_PopPopupsEnabledState\n");
    return false;
}

/*----------------%<---------------------------------------------------------*/

static NPObject * windowClassAllocate (NPP instance, NPClass *aClass) {
    (void)instance;
    /*print ("windowClassAllocate\n");*/
    JsObject * jo = (JsObject *) malloc (sizeof (JsObject));
    memset (jo, 0, sizeof (JsObject));
    jo->npobject._class = aClass;
    return (NPObject *) jo;
}

static void windowClassDeallocate (NPObject *npobj) {
    JsObject *jo = (JsObject *) npobj;
    /*print ("windowClassDeallocate\n");*/
    if (jo->parent) {
        nsReleaseObject ((NPObject *) jo->parent);
    } else if (jo->name && !strncmp (jo->name, "this.__kmplayer__obj_", 21)) {
        char *script = (char *) malloc (strlen (jo->name) + 7);
        char *result;
        char *counter = strrchr (jo->name, '_');
        sprintf (script, "%s=null;", jo->name);
        result = evaluate (script);
        free (script);
        g_free (result);
        if (counter) {
            int c = strtol (counter +1, NULL, 10);
            if (c == js_obj_counter -1)
                js_obj_counter--; /*poor man's variable name reuse */
        }
    }
    if (jo->name)
        g_free (jo->name);
    free (npobj);
}

static void windowClassInvalidate (NPObject *npobj) {
    (void)npobj;
    print ("windowClassInvalidate\n");
}

static bool windowClassHasMethod (NPObject *npobj, NPIdentifier name) {
    (void)npobj; (void)name;
    print ("windowClassHasMehtod\n");
    return false;
}

static bool windowClassInvoke (NPObject *npobj, NPIdentifier method,
        const NPVariant *args, uint32_t arg_count, NPVariant *result) {
    JsObject * jo = (JsObject *) npobj;
    NPString str = { NULL, 0 };
    char buf[512];
    int pos, i;
    bool res;
    char * id = (char *) g_tree_lookup (identifiers, method);
    /*print ("windowClassInvoke\n");*/

    result->type = NPVariantType_Null;
    result->value.objectValue = NULL;

    if (!id) {
        print ("Invoke invalid id\n");
        return false;
    }
    print ("Invoke %s\n", id);
    createJsName (jo, (char **)&str.utf8characters, &str.utf8length);
    pos = snprintf (buf, sizeof (buf), "%s.%s(", str.utf8characters, id);
    free ((char *) str.utf8characters);
    for (i = 0; i < arg_count; i++) {
        (void)args;
        /* TODO: append arguments */
    }
    pos += snprintf (buf + pos, sizeof (buf) - pos, ")");

    str.utf8characters = buf;
    str.utf8length = pos;
    res = nsEvaluate (npp, npobj, &str, result);

    return true;
}

static bool windowClassInvokeDefault (NPObject *npobj,
        const NPVariant *args, uint32_t arg_count, NPVariant *result) {
    (void)npobj; (void)args; (void)arg_count; (void)result;
    print ("windowClassInvokeDefault\n");
    return false;
}

static bool windowClassHasProperty (NPObject *npobj, NPIdentifier name) {
    (void)npobj; (void)name;
    print ("windowClassHasProperty\n");
    return false;
}

static bool windowClassGetProperty (NPObject *npobj, NPIdentifier property,
        NPVariant *result) {
    char * id = (char *) g_tree_lookup (identifiers, property);
    JsObject jo;
    NPString fullname = { NULL, 0 };
    bool res;

    print ("GetProperty %s\n", id);
    result->type = NPVariantType_Null;
    result->value.objectValue = NULL;

    if (!id)
        return false;

    jo.name = id;
    jo.parent = (JsObject *) npobj;
    createJsName (&jo, (char **)&fullname.utf8characters, &fullname.utf8length);

    res = nsEvaluate (npp, npobj, &fullname, result);

    free ((char *) fullname.utf8characters);

    return res;
}

static bool windowClassSetProperty (NPObject *npobj, NPIdentifier property,
        const NPVariant *value) {
    char * id = (char *) g_tree_lookup (identifiers, property);
    char * script = NULL;
    JsObject jo;
    char * fullname = NULL;
    uint32_t len = 0;

    if (!id)
        return false;

    jo.name = id;
    jo.parent = (JsObject *) npobj;
    createJsName (&jo, &fullname, &len);

    print ("SetProperty %s %d\n", id, value->type);
    switch (value->type) {
        case NPVariantType_String:
            script = (char *) malloc (len +
                    value->value.stringValue.utf8length + 5);
            sprintf (script, "%s='%s';",
                    fullname,
                    value->value.stringValue.utf8characters);
            break;
        case NPVariantType_Int32:
            script = (char *) malloc (len + 16);
            sprintf (script, "%s=%d;", fullname, value->value.intValue);
            break;
        case NPVariantType_Double:
            script = (char *) malloc (len + 64);
            sprintf (script, "%s=%f;", fullname, value->value.doubleValue);
            break;
        case NPVariantType_Bool:
            script = (char *) malloc (len + 8);
            sprintf (script, "%s=%s;", fullname,
                    value->value.boolValue ? "true" : "false");
            break;
        case NPVariantType_Null:
            script = (char *) malloc (len + 8);
            sprintf (script, "%s=null;", fullname);
            break;
        case NPVariantType_Object:
            if (&js_class == value->value.objectValue->_class) {
                JsObject *jv = (JsObject *) value->value.objectValue;
                char *val;
                uint32_t vlen = 0;
                createJsName (jv, &val, &vlen);
                script = (char *) malloc (len + vlen + 3);
                sprintf (script, "%s=%s;", fullname, val);
                free (val);
            }
            break;
        default:
            return false;
    }
    if (script) {
        char *res = evaluate (script);
        if (res)
            g_free (res);
        free (script);
    }
    free (fullname);

    return true;
}

static bool windowClassRemoveProperty (NPObject *npobj, NPIdentifier name) {
    (void)npobj; (void)name;
    print ("windowClassRemoveProperty\n");
    return false;
}


/*----------------%<---------------------------------------------------------*/

static void shutDownPlugin() {
    if (scriptable_peer) {
        nsReleaseObject (scriptable_peer);
        scriptable_peer = NULL;
    }
    if (npShutdown) {
        if (npp) {
            np_funcs.destroy (npp, &saved_data);
            free (npp);
            npp = 0L;
        }
        npShutdown();
        npShutdown = 0;
    }
}

static void removeStream (NPReason reason) {
    StreamInfo *si= (StreamInfo *) g_slist_nth_data (stream_list, 0);

    if (stdin_read_watch)
        gdk_input_remove (stdin_read_watch);
    stdin_read_watch = 0;

    if (si) {
        print ("data received %d\n", si->stream_pos);
        np_funcs.destroystream (npp, &si->np_stream, reason);
        if (si->notify)
            np_funcs.urlnotify (npp, si->url, reason, si->notify_data);
        freeStream (si);
        si = (StreamInfo*) g_slist_nth_data (stream_list, 0);
        if (si && callback_service)
            g_timeout_add (0, requestStream, si);
    }
}

static StreamInfo *addStream (const char *url, const char *mime, const char *target, void *notify_data, int notify) {
    StreamInfo *si = (StreamInfo*) g_slist_nth_data (stream_list, 0);
    int req = !si ? 1 : 0;
    si = (StreamInfo *) malloc (sizeof (StreamInfo));

    print ("add stream\n");
    memset (si, 0, sizeof (StreamInfo));
    si->url = g_strdup (url);
    si->np_stream.url = si->url;
    if (mime)
        si->mimetype = g_strdup (mime);
    if (target)
        si->target = g_strdup (target);
    si->notify_data = notify_data;
    si->notify = notify;
    stream_list = g_slist_append (stream_list, si);

    if (req)
        g_timeout_add (0, requestStream, si);

    return si;
}

static void readStdin (gpointer d, gint src, GdkInputCondition cond) {
    StreamInfo *si = (StreamInfo*) g_slist_nth_data (stream_list, 0);
    gsize count = read (src,
            stream_buf + si->stream_buf_pos,
            sizeof (stream_buf) - si->stream_buf_pos);
    (void)d; (void)cond;
    g_assert (si);
    if (count > 0) {
        int32 sz;
        si->stream_buf_pos += count;
        sz = np_funcs.writeready (npp, &si->np_stream);
        if (sz > 0) {
            sz = np_funcs.write (npp, &si->np_stream, si->stream_pos,
                    si->stream_buf_pos > sz ? sz : si->stream_buf_pos,
                    stream_buf);
            if (sz == si->stream_buf_pos)
                si->stream_buf_pos = 0;
            else if (sz > 0) {
                si->stream_buf_pos -= sz;
                memmove (stream_buf, stream_buf + sz, si->stream_buf_pos);
            } else {
            }
            si->stream_pos += sz > 0 ? sz : 0;
        } else {
        }
        if (si->stream_pos == si->total) {
            if (si->stream_pos)
                removeStream (si->reason);
            else
                g_timeout_add (0, destroyStream, si);
        }
    } else {
        removeStream (NPRES_DONE); /* only for 'cat foo | knpplayer' */
    }
}

static int initPlugin (const char *plugin_lib) {
    NPNetscapeFuncs ns_funcs;
    NPError np_err;

    print ("starting %s with %s\n", plugin_lib, object_url);
    library = g_module_open (plugin_lib, G_MODULE_BIND_LAZY);
    if (!library) {
        print ("failed to load %s\n", plugin_lib);
        return -1;
    }
    if (!g_module_symbol (library,
                "NP_GetMIMEDescription", (gpointer *)&npGetMIMEDescription)) {
        print ("undefined reference to load NP_GetMIMEDescription\n");
        return -1;
    }
    if (!g_module_symbol (library,
                "NP_Initialize", (gpointer *)&npInitialize)) {
        print ("undefined reference to load NP_Initialize\n");
        return -1;
    }
    if (!g_module_symbol (library,
                "NP_Shutdown", (gpointer *)&npShutdown)) {
        print ("undefined reference to load NP_Shutdown\n");
        return -1;
    }
    print ("startup succeeded %s\n", npGetMIMEDescription ());
    memset (&ns_funcs, 0, sizeof (NPNetscapeFuncs));
    ns_funcs.size = sizeof (NPNetscapeFuncs);
    ns_funcs.version = (NP_VERSION_MAJOR << 8) + NP_VERSION_MINOR;
    ns_funcs.geturl = nsGetURL;
    ns_funcs.posturl = nsPostURL;
    ns_funcs.requestread = nsRequestRead;
    ns_funcs.newstream = nsNewStream;
    ns_funcs.write = nsWrite;
    ns_funcs.destroystream = nsDestroyStream;
    ns_funcs.status = nsStatus;
    ns_funcs.uagent = nsUserAgent;
    ns_funcs.memalloc = nsAlloc;
    ns_funcs.memfree = nsMemFree;
    ns_funcs.memflush = nsMemFlush;
    ns_funcs.reloadplugins = nsReloadPlugins;
    ns_funcs.getJavaEnv = nsGetJavaEnv;
    ns_funcs.getJavaPeer = nsGetJavaPeer;
    ns_funcs.geturlnotify = nsGetURLNotify;
    ns_funcs.posturlnotify = nsPostURLNotify;
    ns_funcs.getvalue = nsGetValue;
    ns_funcs.setvalue = nsSetValue;
    ns_funcs.invalidaterect = nsInvalidateRect;
    ns_funcs.invalidateregion = nsInvalidateRegion;
    ns_funcs.forceredraw = nsForceRedraw;
    ns_funcs.getstringidentifier = nsGetStringIdentifier;
    ns_funcs.getstringidentifiers = nsGetStringIdentifiers;
    ns_funcs.getintidentifier = nsGetIntIdentifier;
    ns_funcs.identifierisstring = nsIdentifierIsString;
    ns_funcs.utf8fromidentifier = nsUTF8FromIdentifier;
    ns_funcs.intfromidentifier = nsIntFromIdentifier;
    ns_funcs.createobject = nsCreateObject;
    ns_funcs.retainobject = nsRetainObject;
    ns_funcs.releaseobject = nsReleaseObject;
    ns_funcs.invoke = nsInvoke;
    ns_funcs.invokeDefault = nsInvokeDefault;
    ns_funcs.evaluate = nsEvaluate;
    ns_funcs.getproperty = nsGetProperty;
    ns_funcs.setproperty = nsSetProperty;
    ns_funcs.removeproperty = nsRemoveProperty;
    ns_funcs.hasproperty = nsHasProperty;
    ns_funcs.hasmethod = nsHasMethod;
    ns_funcs.releasevariantvalue = nsReleaseVariantValue;
    ns_funcs.setexception = nsSetException;
    ns_funcs.pushpopupsenabledstate = nsPushPopupsEnabledState;
    ns_funcs.poppopupsenabledstate = nsPopPopupsEnabledState;

    js_class.structVersion = NP_CLASS_STRUCT_VERSION;
    js_class.allocate = windowClassAllocate;
    js_class.deallocate = windowClassDeallocate;
    js_class.invalidate = windowClassInvalidate;
    js_class.hasMethod = windowClassHasMethod;
    js_class.invoke = windowClassInvoke;
    js_class.invokeDefault = windowClassInvokeDefault;
    js_class.hasProperty = windowClassHasProperty;
    js_class.getProperty = windowClassGetProperty;
    js_class.setProperty = windowClassSetProperty;
    js_class.removeProperty = windowClassRemoveProperty;

    np_funcs.size = sizeof (NPPluginFuncs);

    np_err = npInitialize (&ns_funcs, &np_funcs);
    if (np_err != NPERR_NO_ERROR) {
        print ("NP_Initialize failure %d\n", np_err);
        npShutdown = 0;
        return -1;
    }
    return 0;
}

static int newPlugin (NPMIMEType mime, int16 argc, char *argn[], char *argv[]) {
    NPSetWindowCallbackStruct ws_info;
    NPError np_err;
    Display *display;
    int screen;
    int i;
    int needs_xembed;
    unsigned int width = 320, height = 240;

    npp = (NPP_t*)malloc (sizeof (NPP_t));
    memset (npp, 0, sizeof (NPP_t));
    for (i = 0; i < argc; i++) {
        if (!strcasecmp (argn[i], "width"))
            width = strtol (argv[i], 0L, 10);
        else if (!strcasecmp (argn[i], "height"))
            height = strtol (argv[i], 0L, 10);
    }
    np_err = np_funcs.newp (mime, npp, NP_EMBED, argc, argn, argv, saved_data);
    if (np_err != NPERR_NO_ERROR) {
        print ("NPP_New failure %d %p %p\n", np_err, np_funcs, np_funcs.newp);
        return -1;
    }
    if (np_funcs.getvalue) {
        np_err = np_funcs.getvalue ((void*)npp,
                NPPVpluginNeedsXEmbed, (void*)&needs_xembed);
        if (np_err != NPERR_NO_ERROR || !needs_xembed) {
            print ("NPP_GetValue NPPVpluginNeedsXEmbed failure %d\n", np_err);
            shutDownPlugin();
            return -1;
        }
        np_err = np_funcs.getvalue ((void*)npp,
                NPPVpluginScriptableNPObject, (void*)&scriptable_peer);
        if (np_err != NPERR_NO_ERROR || !scriptable_peer)
            print ("NPP_GetValue no NPPVpluginScriptableNPObject %d\n", np_err);
    }
    memset (&np_window, 0, sizeof (NPWindow));
    display = gdk_x11_get_default_xdisplay ();
    np_window.x = 0;
    np_window.y = 0;
    np_window.width = width;
    np_window.height = height;
    np_window.window = (void*)socket_id;
    np_window.type = NPWindowTypeWindow;
    ws_info.type = NP_SETWINDOW;
    screen = DefaultScreen (display);
    ws_info.display = (void*)(long)display;
    ws_info.visual = (void*)(long)DefaultVisual (display, screen);
    ws_info.colormap = DefaultColormap (display, screen);
    ws_info.depth = DefaultDepth (display, screen);
    print ("display %u %dx%d\n", socket_id, width, height);
    np_window.ws_info = (void*)&ws_info;

    np_err = np_funcs.setwindow (npp, &np_window);

    return 0;
}

static gboolean requestStream (void * p) {
    StreamInfo *si = (StreamInfo*) p;
    print ("requestStream\n");
    if (si && si == g_slist_nth_data (stream_list, 0)) {
        NPError err;
        uint16 stype = NP_NORMAL;
        si->np_stream.notifyData = si->notify_data;
        err = np_funcs.newstream (npp, si->mimetype, &si->np_stream, 0, &stype);
        if (err != NPERR_NO_ERROR) {
            g_printerr ("newstream error %d\n", err);
            freeStream (si);
            return 0;
        }
        g_assert (!stdin_read_watch);
        stdin_read_watch = gdk_input_add (0, GDK_INPUT_READ, readStdin, NULL);
        if (si->target)
            callFunction ("getUrl",
                    DBUS_TYPE_STRING, &si->url,
                    DBUS_TYPE_STRING, &si->target, DBUS_TYPE_INVALID);
        else
            callFunction ("getUrl", DBUS_TYPE_STRING, &si->url, DBUS_TYPE_INVALID);
    }
    return 0; /* single shot */
}

static gboolean destroyStream (void * p) {
    StreamInfo *si = (StreamInfo*) p;
    print ("destroyStream\n");
    if (si && si == g_slist_nth_data (stream_list, 0))
        callFunction ("finish", DBUS_TYPE_INVALID);
    return 0; /* single shot */
}

static gpointer newStream (const char *url, const char *mime,
        int argc, char *argn[], char *argv[]) {
    StreamInfo *si;
    if (!npp && (initPlugin (plugin) || newPlugin (mimetype, argc, argn, argv)))
        return 0L;
    si = addStream (url, mime, 0L, 0L, 0);
    return si;
}

/*----------------%<---------------------------------------------------------*/

static DBusHandlerResult dbusFilter (DBusConnection * connection,
        DBusMessage *msg, void * user_data) {
    DBusMessageIter args;
    const char *sender = dbus_message_get_sender (msg);
    const char *iface = "org.kde.kmplayer.backend";
    (void)user_data; (void)connection;
    if (!dbus_message_has_destination (msg, service_name))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    print ("dbusFilter %s %s\n", sender,dbus_message_get_interface (msg));
    if (dbus_message_is_method_call (msg, iface, "play")) {
        DBusMessageIter ait;
        char *param = 0;
        unsigned int params;
        char **argn = NULL;
        char **argv = NULL;
        int i;
        if (!dbus_message_iter_init (msg, &args) ||
                DBUS_TYPE_STRING != dbus_message_iter_get_arg_type (&args)) {
            g_printerr ("missing url arg");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_get_basic (&args, &param);
        object_url = g_strdup (param);
        if (!dbus_message_iter_next (&args) ||
                DBUS_TYPE_STRING != dbus_message_iter_get_arg_type (&args)) {
            g_printerr ("missing mimetype arg");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_get_basic (&args, &param);
        mimetype = g_strdup (param);
        if (!dbus_message_iter_next (&args) ||
                DBUS_TYPE_STRING != dbus_message_iter_get_arg_type (&args)) {
            g_printerr ("missing plugin arg");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_get_basic (&args, &param);
        plugin = g_strdup (param);
        if (!dbus_message_iter_next (&args) ||
                DBUS_TYPE_UINT32 != dbus_message_iter_get_arg_type (&args)) {
            g_printerr ("missing param count arg");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_get_basic (&args, &params);
        if (params > 0 && params < 100) {
            argn = (char**) malloc (params * sizeof (char *));
            argv = (char**) malloc (params * sizeof (char *));
        }
        if (!dbus_message_iter_next (&args) ||
                DBUS_TYPE_ARRAY != dbus_message_iter_get_arg_type (&args)) {
            g_printerr ("missing params array");
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_recurse (&args, &ait);
        for (i = 0; i < params; i++) {
            char *key, *value;
            DBusMessageIter di;
            if (dbus_message_iter_get_arg_type (&ait) != DBUS_TYPE_DICT_ENTRY)
                break;
            dbus_message_iter_recurse (&ait, &di);
            if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type (&di))
                break;
            dbus_message_iter_get_basic (&di, &key);
            if (!dbus_message_iter_next (&di) ||
                    DBUS_TYPE_STRING != dbus_message_iter_get_arg_type (&di))
                break;
            dbus_message_iter_get_basic (&di, &value);
            argn[i] = g_strdup (key);
            argv[i] = g_strdup (value);
            print ("param %d:%s='%s'\n", i + 1, argn[i], value);
            if (!dbus_message_iter_next (&ait))
                params = i + 1;
        }
        print ("play %s %s %s params:%d\n", object_url, mimetype, plugin, i);
        newStream (object_url, mimetype, i, argn, argv);
    } else if (dbus_message_is_method_call (msg, iface, "getUrlNotify")) {
        StreamInfo *si = (StreamInfo*) g_slist_nth_data (stream_list, 0);
        if (si && dbus_message_iter_init (msg, &args) && 
                DBUS_TYPE_UINT32 == dbus_message_iter_get_arg_type (&args)) {
            dbus_message_iter_get_basic (&args, &si->total);
            if (dbus_message_iter_next (&args) &&
                   DBUS_TYPE_UINT32 == dbus_message_iter_get_arg_type (&args)) {
                dbus_message_iter_get_basic (&args, &si->reason);
                print ("getUrlNotify bytes:%d reason:%d\n", si->total, si->reason);
                if (si->stream_pos == si->total)
                    removeStream (si->reason);
            }
        }
    } else if (dbus_message_is_method_call (msg, iface, "quit")) {
        print ("quit\n");
        shutDownPlugin();
        gtk_main_quit();
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

static void callFunction(const char *func, int first_arg_type, ...) {
    print ("call %s()\n", func);
    if (callback_service) {
        va_list var_args;
        DBusMessage *msg = dbus_message_new_method_call (
                callback_service,
                callback_path,
                "org.kde.kmplayer.callback",
                func);
        if (first_arg_type != DBUS_TYPE_INVALID) {
            va_start (var_args, first_arg_type);
            dbus_message_append_args_valist (msg, first_arg_type, var_args);
            va_end (var_args);
        }
        dbus_message_set_no_reply (msg, TRUE);
        dbus_connection_send (dbus_connection, msg, NULL);
        dbus_message_unref (msg);
        dbus_connection_flush (dbus_connection);
    }
}

static char * evaluate (const char *script) {
    char * ret = NULL;
    print ("evaluate %s\n", script);
    if (callback_service) {
        DBusMessage *rmsg;
        DBusMessage *msg = dbus_message_new_method_call (
                callback_service,
                callback_path,
                "org.kde.kmplayer.callback",
                "evaluate");
        dbus_message_append_args (
                msg, DBUS_TYPE_STRING, &script, DBUS_TYPE_INVALID);
        rmsg = dbus_connection_send_with_reply_and_block (dbus_connection,
                msg, 2000, NULL);
        if (rmsg) {
            DBusMessageIter it;
            if (dbus_message_iter_init (rmsg, &it) &&
                    DBUS_TYPE_STRING == dbus_message_iter_get_arg_type (&it)) {
                char * param;
                dbus_message_iter_get_basic (&it, &param);
                ret = g_strdup (param);
            }
            dbus_message_unref (rmsg);
            print ("  => %s\n", ret);
        }
        dbus_message_unref (msg);
    }
    return ret;
}

/*----------------%<---------------------------------------------------------*/

static void pluginAdded (GtkSocket *socket, gpointer d) {
    /*(void)socket;*/ (void)d;
    print ("pluginAdded\n");
    if (socket->plug_window) {
        gpointer user_data = NULL;
        gdk_window_get_user_data (socket->plug_window, &user_data);
        if (!user_data) {
            /**
             * GtkSocket resets plugins XSelectInput in
             * _gtk_socket_add_window
             *   _gtk_socket_windowing_select_plug_window_input
             **/
            XSelectInput (gdk_x11_get_default_xdisplay (),
                    gdk_x11_drawable_get_xid (socket->plug_window),
                    KeyPressMask | KeyReleaseMask |
                    ButtonPressMask | ButtonReleaseMask |
                    KeymapStateMask |
                    ButtonMotionMask |
                    PointerMotionMask |
                    EnterWindowMask | LeaveWindowMask |
                    FocusChangeMask |
                    ExposureMask |
                    StructureNotifyMask |
                    SubstructureRedirectMask |
                    PropertyChangeMask
                    );
        }
    }
    callFunction ("plugged", DBUS_TYPE_INVALID);
}

static void windowCreatedEvent (GtkWidget *w, gpointer d) {
    (void)d;
    print ("windowCreatedEvent\n");
    socket_id = gtk_socket_get_id (GTK_SOCKET (xembed));
    if (parent_id) {
        print ("windowCreatedEvent %p\n", GTK_PLUG (w)->socket_window);
        if (!GTK_PLUG (w)->socket_window)
            gtk_plug_construct (GTK_PLUG (w), parent_id);
        gdk_window_reparent( w->window,
                GTK_PLUG (w)->socket_window
                    ? GTK_PLUG (w)->socket_window
                    : gdk_window_foreign_new (parent_id),
                0, 0);
        gtk_widget_show_all (w);
        /*XReparentWindow (gdk_x11_drawable_get_xdisplay (w->window),
                gdk_x11_drawable_get_xid (w->window),
                parent_id,
                0, 0);*/
    }
    if (!callback_service) {
        char *argn[] = { "WIDTH", "HEIGHT", "debug", "SRC" };
        char *argv[] = { "440", "330", g_strdup("yes"), g_strdup(object_url) };
        newStream (object_url, mimetype, 4, argn, argv);
    }
}

static void embeddedEvent (GtkPlug *plug, gpointer d) {
    (void)plug; (void)d;
    print ("embeddedEvent\n");
}

static gboolean configureEvent(GtkWidget *w, GdkEventConfigure *e, gpointer d) {
    (void)w; (void)d;
    if (np_window.window &&
            (e->width != np_window.width || e->height != np_window.height)) {
        print ("Update size %dx%d\n", e->width, e->height);
        np_window.width = e->width;
        np_window.height = e->height;
        np_funcs.setwindow (npp, &np_window);
    }
    return FALSE;
}

static gboolean windowCloseEvent (GtkWidget *w, GdkEvent *e, gpointer d) {
    (void)w; (void)e; (void)d;
    shutDownPlugin();
    return FALSE;
}

static void windowDestroyEvent (GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    gtk_main_quit();
}

static gboolean initPlayer (void * p) {
    GtkWidget *window;
    GdkColormap *color_map;
    GdkColor bg_color;
    (void)p;

    window = callback_service
        ? gtk_plug_new (parent_id)
        : gtk_window_new (GTK_WINDOW_TOPLEVEL);
    g_signal_connect (G_OBJECT (window), "delete_event",
            G_CALLBACK (windowCloseEvent), NULL);
    g_signal_connect (G_OBJECT (window), "destroy",
            G_CALLBACK (windowDestroyEvent), NULL);
    g_signal_connect_after (G_OBJECT (window), "realize",
            GTK_SIGNAL_FUNC (windowCreatedEvent), NULL);
    /*g_signal_connect (G_OBJECT (window), "configure-event",
            GTK_SIGNAL_FUNC (configureEvent), NULL);*/

    xembed = gtk_socket_new();
    g_signal_connect (G_OBJECT (xembed), "plug-added",
            GTK_SIGNAL_FUNC (pluginAdded), NULL);

    color_map = gdk_colormap_get_system();
    gdk_colormap_query_color (color_map, 0, &bg_color);
    gtk_widget_modify_bg (xembed, GTK_STATE_NORMAL, &bg_color);

    gtk_container_add (GTK_CONTAINER (window), xembed);

    if (!parent_id) {
        gtk_widget_set_size_request (window, 440, 330);
        gtk_widget_show_all (window);
    } else {
        g_signal_connect (G_OBJECT (window), "embedded",
                GTK_SIGNAL_FUNC (embeddedEvent), NULL);
        gtk_widget_realize (window);
    }

    if (callback_service && callback_path) {
        DBusError dberr;
        const char *serv = "type='method_call',interface='org.kde.kmplayer.backend'";
        char myname[64];

        dbus_error_init (&dberr);
        dbus_connection = dbus_bus_get (DBUS_BUS_SESSION, &dberr);
        if (!dbus_connection) {
            g_printerr ("Failed to open connection to bus: %s\n",
                    dberr.message);
            exit (1);
        }
        g_sprintf (myname, "org.kde.kmplayer.npplayer-%d", getpid ());
        service_name = g_strdup (myname);
        print ("using service %s was '%s'\n", service_name, dbus_bus_get_unique_name (dbus_connection));
        dbus_connection_setup_with_g_main (dbus_connection, 0L);
        dbus_bus_request_name (dbus_connection, service_name, 
                DBUS_NAME_FLAG_REPLACE_EXISTING, &dberr);
        if (dbus_error_is_set (&dberr)) {
            g_printerr ("Failed to register name: %s\n", dberr.message);
            dbus_connection_unref (dbus_connection);
            return -1;
        }
        dbus_bus_add_match (dbus_connection, serv, &dberr);
        if (dbus_error_is_set (&dberr)) {
            g_printerr ("dbus_bus_add_match error: %s\n", dberr.message);
            dbus_connection_unref (dbus_connection);
            return -1;
        }
        dbus_connection_add_filter (dbus_connection, dbusFilter, 0L, 0L);

        /* TODO: remove DBUS_BUS_SESSION and create a private connection */
        callFunction ("running", DBUS_TYPE_STRING, &service_name, DBUS_TYPE_INVALID);

        dbus_connection_flush (dbus_connection);
    }
    return 0; /* single shot */
}

int main (int argc, char **argv) {
    int i;
    gtk_init (&argc, &argv);

    for (i = 1; i < argc; i++) {
        if (!strcmp (argv[i], "-p") && ++i < argc) {
            plugin = g_strdup (argv[i]);
        } else if (!strcmp (argv[i], "-cb") && ++i < argc) {
            gchar *cb = g_strdup (argv[i]);
            gchar *path = strchr(cb, '/');
            if (path) {
                callback_path = g_strdup (path);
                *path = 0;
            }
            callback_service = g_strdup (cb);
            g_free (cb);
        } else if (!strcmp (argv[i], "-m") && ++i < argc) {
            mimetype = g_strdup (argv[i]);
        } else if (!strcmp (argv [i], "-wid") && ++i < argc) {
            parent_id = strtol (argv[i], 0L, 10);
        } else
            object_url = g_strdup (argv[i]);
    }
    if (!callback_service && !(object_url && mimetype && plugin)) {
        g_fprintf(stderr, "Usage: %s <-m mimetype -p plugin url|-cb service -wid id>\n", argv[0]);
        return 1;
    }

    identifiers = g_tree_new (strcmp);

    g_timeout_add (0, initPlayer, NULL);

    fcntl (0, F_SETFL, fcntl (0, F_GETFL) | O_NONBLOCK);

    print ("entering gtk_main\n");

    gtk_main();

    if (dbus_connection)
        dbus_connection_unref (dbus_connection);

    return 0;
}
