#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <config.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include <cups/cups.h>
#include <cups/http.h>
#include <cups/ipp.h>

#include <dbus/dbus.h>

//TODO: localize messages
//TODO: change dbus message strings
//TODO: use other backends

#define CUPS_BACKEND_DIR LIBDIR "/cups/backend"
#define LOGFILE "/var/log/cups-autoconfig.log"

#define dbg(fmt,arg...) log_it(TRUE, fmt, ##arg)
#define err(fmt,arg...) log_it(FALSE, fmt, ##arg)

typedef enum {
    PPD_NO_MATCH,
    PPD_MATCH,
    PPD_RECOMMENDED,
    PPD_MANUFACTURER
} PPDScore;

typedef struct _PrinterInfo {
    gchar *uri;
    gchar *name;
    gchar *make;
    gchar *model;
    gchar *make_and_model;
    gchar *device_id;
    gchar *serial;
} PrinterInfo;

FILE *log_file;
GHashTable *vendor_map;
GHashTable *alias_map;
http_t *global_cups_connection;
gboolean enable_debug = FALSE;

static void log_it (gboolean debug, const char *fmt, ...)
{
    va_list args;

    va_start (args, fmt);
    g_vfprintf (stderr, fmt, args);
    g_vfprintf (log_file, fmt, args);
    if (enable_debug && debug)
        g_vfprintf (log_file, fmt, args);
    va_end (args);
}

static void free_printer_info (gpointer data, gpointer user_data)
{
    PrinterInfo *pi = data;
    g_free (pi->name);
    g_free (pi->uri);
    g_free (pi->make);
    g_free (pi->model);
    g_free (pi->make_and_model);
    g_free (pi->device_id);
    g_free (pi->serial);
}

static gboolean open_log (void)
{
    log_file = fopen (LOGFILE, "w");
    if (!log_file) {
        g_printerr ("Failed to open log file: %s\n", strerror (errno));
        return FALSE;
    }

    return TRUE;
}

/*
 * Load our known vendor string mappings.
 */
static void load_vendor_mappings (void)
{
    gint i;
    static char const *okidata[] = { "OKI DATA CORP", "OKI", NULL };
    static char const *minolta[] = { "MINOLTA-QMS", "MINOLTA QMS", "KONICA MINOLTA", NULL };
    static char const *lexmark[] = { "Lexmark-International", "Lexmark International", NULL };
    static char const *kyocera[] = { "Kyocera-Mita", "Kyocera Mita", NULL };
    static char const *hp[] = { "Hewlett-Packard", "Hewlett Packard", NULL };
    static char const *dymo[] = { "Dymo-CoStar", NULL };
    static char const *canon[] = { "Canon Inc. (Kosugi Offic", NULL };
    static char const *generic[] = { "Raw Queue", "Postscript", NULL };
    
    struct {
        const char *key;
        const char *val;
    } vendor_mappings[] = {
        { "OKI DATA CORP", "Okidata" }, 
        { "OKI", "Okidata" }, 
        { "MINOLTA-QMS", "Minolta" },
        { "MINOLTA QMS", "Minolta" },
        { "LEXMARK-INTERNATIONAL", "Lexmark" },
        { "LEXMARK INTERNATIONAL", "Lexmark" },
        { "KYOCERA-MITA", "Kyocera" },
        { "KYOCERA MITA", "Kyocera" },
        { "HEWLETT-PACKARD", "HP" },
        { "HEWLETT PACKARD", "HP" },
        { "DYMO-COSTAR", "Dymo" }, 
        { "CANON INC. (KOSUGI OFFIC", "Canon" }, 
        { "KONICA MINOLTA", "Minolta" }, 
        { "RAW QUEUE", "Generic" },
        { "POSTSCRIPT", "Generic" },
        { NULL, NULL }
    };

    vendor_map = g_hash_table_new (g_str_hash, g_str_equal);
    for (i = 0; vendor_mappings[i].key; i++) {
        g_hash_table_insert (vendor_map,
                            (gpointer) vendor_mappings[i].key,
                            (gpointer) vendor_mappings[i].val);
    }

    alias_map = g_hash_table_new (g_str_hash, g_str_equal);
    g_hash_table_insert (alias_map, "OKIDATA", okidata);
    g_hash_table_insert (alias_map, "MINOLTA", minolta);
    g_hash_table_insert (alias_map, "LEXMARK", lexmark);
    g_hash_table_insert (alias_map, "KYOCERA", kyocera);
    g_hash_table_insert (alias_map, "HP", hp);
    g_hash_table_insert (alias_map, "DYMO", dymo);
    g_hash_table_insert (alias_map, "CANON", canon);
    g_hash_table_insert (alias_map, "GENERIC", generic);
}

/* 
 * Connect to cups.
 */
static gboolean cups_connect (void)
{
    global_cups_connection = httpConnectEncrypt (cupsServer (), ippPort (), cupsEncryption ());
    if (!global_cups_connection) {
        err ("Failed to connect to cupsd\n");
        return FALSE;
    }

    return TRUE;
}

/*
 * Disconnect from cups.
 */
static void cups_disconnect (void)
{
    if (global_cups_connection)
        httpClose (global_cups_connection);
}

/*
 * Send a dbus message out on the system bus.
 */
static gboolean send_dbus_message (DBusMessage *msg)
{
    DBusConnection *conn;
    DBusError error;

    dbus_error_init (&error);
    conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
    if (!conn) {
        err ("Failed to connect to dbus: %s\n", error.message);
        dbus_error_free (&error);
        dbus_message_unref (msg);
        return FALSE;
    }

    if (!dbus_connection_send (conn, msg, NULL)) {
        err ("Failed to send DBUS message\n");
        dbus_message_unref (msg);
        dbus_connection_unref (conn);
        return FALSE;
    }

    dbus_connection_flush (conn);
    dbus_message_unref (msg);
    dbus_connection_unref (conn);
    return TRUE;
}

/*
 * Let the user know that we did something.
 */
static gboolean send_notification (const gchar *title, const gchar *message)
{
    DBusMessage *msg;
    DBusMessageIter iter;

    msg = dbus_message_new_signal ("/org/cups/CupsAutoconfig",
                                   "org.cups.CupsAutoconfig",
                                   "PrinterInfo");
    if (!msg) {
        err ("Failed to construct message\n");
        return FALSE;
    }

    dbg ("Sending PrinterInfo message: %s - %s\n", title, message);
    dbus_message_iter_init_append (msg, &iter);
    dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &title);
    dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &message);
    return send_dbus_message (msg); 
}

/*
 * Send a signal that policy apps can watch for to
 * run their current printer configuration tool.
 */
static gboolean request_manual_configuration (const gchar *udi)
{
    DBusMessage *msg;
    DBusMessageIter iter;

    msg = dbus_message_new_signal ("/org/cups/CupsAutoconfig",
                                   "org.cups.CupsAutoconfig",
                                   "PrinterConfigRequired");
    if (!msg) {
        err ("Failed to construct message\n");
        return FALSE;
    }

    dbg ("Sending PrinterConfigRequired message with udi '%s'\n", udi);
    dbus_message_iter_init_append (msg, &iter);
    dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &udi);
    return send_dbus_message (msg); 
}

/* 
 * Generate a unique name for a new printer.  The returned string 
 * needs to be freed by the caller. 
 */
static gchar *generate_printer_name (PrinterInfo *pi, GSList *curr)
{
    GSList *l = NULL;
    gchar *ret = g_strdup (pi->make_and_model);
    size_t len = strlen (ret);
    gboolean found;
    gint i;

    for (i = 0; i < len; i++) {
        if (ret[i] == ' ' || ret[i] == '\\' || ret[i] == '#')
            ret[i] = '_';
    }

    i = 0;
    do {
        found = FALSE;
        if (i++) {
            gchar *tmp;
            tmp = g_strdup_printf ("%s-%d", ret, i);
            g_free (ret);
            ret = tmp;
        }

        for (l = curr; l; l = l->next) {
            PrinterInfo *p = l->data;
            if (!g_ascii_strcasecmp (p->name, ret)) {
                found = TRUE;
                break;
            }
        }
    } while (found);
    return ret;
}

/*
 * A crappy check to make sure a printer uri is local.
 */
static gboolean uri_is_local (const gchar *uri)
{
    g_return_val_if_fail (uri, FALSE);
    return (strstr (uri, "usb:/") || strstr (uri, "hp:/") ||
            strstr (uri, "epson:/")) ? TRUE : FALSE;
}

/*
 * Remove the make from the make and model string, if it's there.  The
 * returned string needs to be freed by the caller.
 */
static gchar *model_from_string (const gchar *make_str, const gchar *model_str)
{
    const char *crap[] = { ",", "(", "FOOMATIC/", "- CUPS", "CUPS", "(RECOMMEND",
                           " POSTSCRIPT ", " PS3 ", " PS -", " PS V", "W/PS" };
    gchar *make, *ss = NULL, *mdl = NULL;
    gsize ss_len;
    int i;

    g_return_val_if_fail (model_str, NULL);
    mdl = g_ascii_strup (model_str, -1);
    make = g_strstrip (g_ascii_strup (make_str, -1));
    
    /* strip off known crap from the model field */
    for (i = 0; i < sizeof(crap) / sizeof(crap[0]); i++) {
        ss = strstr (mdl, crap[i]);
        if (ss)
            *ss = '\0';
    }

    /* strip off spaces */
    mdl = g_strstrip (mdl);
    
    /* look for the make or any of its aliases */
    ss_len = strlen (make);
    ss = strstr (mdl, make);
    if (!ss) {
        /* see if we have an alias in the string */
        const gchar **alias = g_hash_table_lookup (alias_map, make);
        if (!alias)
            goto done;

        for (; *alias; alias++) {
            ss = strstr (mdl, *alias);
            if (ss) {
                ss_len = strlen (*alias);
                break;
            }
        }
    }

    /* we need strip make or an alias out */
    if (ss) {
        gchar *p = ss + ss_len;
        gchar *tmp = mdl;
        mdl = g_strdup (p);
        g_free (tmp);
    }

    /* strip off spaces one last time */
    mdl = g_strstrip (mdl);

done:
    g_free (make);
    return mdl;
}

/*
 * Get the model from an ieee 1284 id string.  The model string
 * needs to be freed by the caller.
 */
static gchar *model_from_1284_id (const gchar *id)
{
    gchar *s, *e, *ret = NULL;
    gchar *uid = g_ascii_strup (id, -1);
  
    if ((s = strstr (uid, "MDL:")))
        s += 4;
    else if ((s = strstr (uid, "MODEL:")))
        s += 6; 
    else
        goto done;
    
    e = strchr (s, ';');
    if (!e)
        goto done;

    *e = '\0';
    ret = g_strstrip (g_strdup (s));
    
done:
    g_free (uid);
    return ret;
}

/*
 * See if the HAL properties match the properties for the
 * given printer.  We check the environment variables HAL
 * exports for its callouts.
 */
static gboolean printer_matches_hal_properties (PrinterInfo *pi)
{
    gchar *make = g_strdup (g_getenv ("HAL_PROP_PRINTER_VENDOR"));
    gchar *model = g_strdup (g_getenv ("HAL_PROP_PRINTER_PRODUCT"));
    gchar *serial = g_strdup (g_getenv ("HAL_PROP_PRINTER_SERIAL"));
    gchar *mm, *um, *mdl = NULL;
    gboolean ret = FALSE;

    dbg ("HAL env properties make='%s' model='%s' serial='%s'\n"
         "Printer properties uri='%s' m_and_m='%s'\n", 
         make, model, serial, pi->uri, pi->make_and_model);

    if (!model || !make)
        goto done;

    if (!pi->uri)
        goto done;

    /* check the serial numbers if they're there */
    if (serial) {
        gchar *ser = strstr (pi->uri, "?serial=");
        if (ser) {
            ser += 8;
            if (!strcmp (ser, serial)) {
                ret = TRUE;
                goto matched;
            }
        } else
            dbg ("couldn't find a serial number in the backend uri\n");
    }

    /* strip spaces */
    make = g_strstrip (make);
    model = g_strstrip (model);

    /* check our vendor mappings */
    um = g_ascii_strup (make, -1);
    mm = g_hash_table_lookup (vendor_map, um);
    if (mm)
        make = mm;

    /* check the model */
    mdl = model_from_string (make, pi->make_and_model);
    if (g_ascii_strcasecmp (mdl, model)) {
        dbg ("models '%s' and '%s' didn't match with make '%s'\n",
             mdl, model, make);
        goto done;
    }

matched:
    pi->make = g_strdup (make);
    pi->model = g_strdup (model);
    pi->serial = g_strdup (serial);
    ret = TRUE;

done:
    g_free (mdl);
    g_free (make);
    g_free (model);
    g_free (serial);
    return ret;
}

/*
 * Return the best PPD file to use for a given printer.  The
 * returned string must be freed by the caller.
 */
static gchar *get_best_ppd (PrinterInfo *pi)
{
    gchar *ppd = NULL;
    ipp_t *request, *response;
    ipp_attribute_t *attr;
    PPDScore ppd_score = PPD_NO_MATCH;

    /* get the list of ppds for pi's vendor */
    dbg ("Requesting ppds for vendor '%s'\n", pi->make);
    request = ippNewRequest (CUPS_GET_PPDS);
    ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_TEXT,
		          "ppd-make", NULL, pi->make);
    response = cupsDoRequest (global_cups_connection, request, "/");
    if (!response || response->request.status.status_code > IPP_OK_CONFLICT) {
        err ("Failed to get ppds for '%s'\n", pi->make);
        goto done;
    } 

    /* 
     * Look for ppds that match our model with preference towards 
     * ppds located in the manufacturer-PPDs directory.
     */
    for (attr = response->attrs; attr; attr = attr ? attr->next : NULL) {
        gchar *ppd_model = NULL;
        const gchar *name = NULL, *make_and_model = NULL, *id = NULL;
        gboolean match = FALSE;
        
        while (attr && attr->group_tag != IPP_TAG_PRINTER)
            attr = attr->next;

        if (!attr)
            goto done;

        for (; attr && attr->group_tag == IPP_TAG_PRINTER; attr = attr->next) {
            if (!strcmp (attr->name, "ppd-name") && attr->value_tag == IPP_TAG_NAME) {
                name = attr->values[0].string.text;
            } else if (!strcmp (attr->name, "ppd-make-and-model") && attr->value_tag == IPP_TAG_TEXT) {
                make_and_model = attr->values[0].string.text;
            } else if (!strcmp (attr->name, "ppd-device-id") && attr->value_tag == IPP_TAG_TEXT) {
                id = attr->values[0].string.text;
            } 
        }

        if (pi->device_id && id && strlen (id)) {
            gchar *pm;

            /* match with ieee 1284 ids */
            dbg ("Matching with 1284 ids '%s' and '%s'\n", pi->device_id, id);
            pm = model_from_1284_id (pi->device_id);
            ppd_model = model_from_1284_id (id);
            dbg ("Extracted models are '%s' and '%s'\n", pm, ppd_model);
            if (!ppd_model || !pm)
                continue;

            match = !g_ascii_strcasecmp (ppd_model, pm) ? TRUE : FALSE;
            dbg ("Result for matching '%s' and '%s' was %d\n\n", ppd_model, pm, match);
        } else {
            /* match with model strings */
            dbg ("Matching with model strings '%s' and '%s'\n", pi->model, make_and_model);
            ppd_model = model_from_string (pi->make, make_and_model);
            dbg ("Extracted model string from ppd was '%s'\n", ppd_model);
            if (!ppd_model)
                continue;

            match = g_ascii_strcasecmp (ppd_model, pi->model) ? FALSE : TRUE;
            dbg ("Result for matching '%s' and '%s' was %d\n\n", ppd_model, pi->model, match);
        }

        if (match) {
            if (strstr (name, "manufacturer-PPDs")) {
                if (ppd)
                    g_free (ppd);
                ppd = g_strdup (name);
                goto done;
            } else if (strstr (make_and_model, "(recommended)")) {
                if (ppd_score < PPD_RECOMMENDED) {
                    if (ppd)
                        g_free (ppd);
                    ppd = g_strdup (name);
                    ppd_score = PPD_RECOMMENDED;
                }
            } else {
                if (ppd_score < PPD_MATCH) {
                    if (ppd)
                        g_free (ppd);
                    ppd = g_strdup (name);
                    ppd_score = PPD_MATCH;
                }
            }
        }

        g_free (ppd_model);
    }

done:
    ippDelete (response);
    return ppd;
}

/*
 * Get the list of detected printers from a cups backend.
 */
static gboolean get_local_printers (GSList **list, const gchar *backend)
{
    gchar *path = g_build_path ("/", CUPS_BACKEND_DIR, backend, NULL);
    gchar *argv[] = { path, NULL };
    GError *err = NULL;
    FILE *fp;
    gboolean ret = FALSE;
    GPid child;
    gint status;
    gint std_out;
    gchar buff[512];
    
    ret = g_spawn_async_with_pipes (NULL, argv, NULL,
                                    G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL,
                                    NULL, NULL, &child, NULL, &std_out, NULL, &err);
    if (!ret) {
        err ("%s\n", err->message);
        g_error_free (err);
        g_free (path);
        return FALSE;
    }

    g_free (path);
    fp = fdopen (std_out, "r");
    if (!fp) {
        err ("%s\n", strerror (errno));
        return FALSE;
    }
    
    while (fgets (buff, 512, fp)) {
        PrinterInfo *pi;
        gchar *start, *end, *uri, *p;
        gsize uri_len;
        gint i;

        /* all local printers have direct as the first field */
        if (strncmp ("direct", buff, 6))
            continue;

        start = buff + 7;

        /* skip the dummy usb devices */
        if (!strncmp ("usb:/dev", start, 8))
            continue;
        
        /* get the uri */
        end = strstr (start, " ");
        if (!end)
            continue;

        *end = '\0';

        /* make sure it's a valid uri for this backend */
        uri = g_strconcat (backend, "://", NULL);
        uri_len = strlen (uri); 
        if (strncmp (start, uri, uri_len)) {
            g_free (uri);
            continue;
        }

        g_free (uri);
        pi = g_new0 (PrinterInfo, 1);
        pi->uri = g_strdup (start);
        
        /* look for make and model */
        start = strchr (end + 1, '"');
        if (!start) {
            free_printer_info (pi, NULL);
            continue;
        }

        start++;
        end = strchr (start + 1, '"');
        if (!end) {
            free_printer_info (pi, NULL);
            continue;
        }

        *(end++) = '\0';
        pi->make_and_model = g_strdup (start);
       
        /* look for the device-id, which is optional */
        for (i = 0, p = end + 1; *p != '\0'; p++) {
            if (*p != '"')
                continue;

            i++;
            if (i == 3) {
                start = p;
            } else if (i == 4) {
                *(p + 1) = '\0';
                pi->device_id = g_strdup (start);
                break;
            }
        }

        *list = g_slist_append (*list, pi);
        dbg ("detected local printer '%s' - '%s'\n",
             pi->uri, pi->make_and_model);
    }

    fclose (fp);

    if (waitpid (child, &status, 0) == -1 ||
        WEXITSTATUS (status) != 0)
        ret = FALSE;

    g_spawn_close_pid (child);
    return ret;
}

/*
 * Get the printers that the cups backends detect.
 */
//FIXME: use the hp and epson backends
static gboolean get_detected_printers (GSList **list)
{
    int i;
    //const char *backends[] = { "usb", "hp", "epson" };
    const char *backends[] = { "usb" };
    
    for (i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (!get_local_printers (list, backends[i]))
            err ("Failed to get printers from backend '%s'\n", backends[i]);
    }

    return TRUE;
}

/*
 * Get the list of printers already added to cups.
 */
static gboolean get_cups_printers (GSList **list)
{
    ipp_t *request, *response;
    ipp_attribute_t *attr;

    request = ippNewRequest (CUPS_GET_PRINTERS);
    response = cupsDoRequest (global_cups_connection, request, "/");
    if (!response || response->request.status.status_code > IPP_OK_CONFLICT) {
        err ("Failed to get the list of printers from cupsd\n");
        return FALSE;
    }

    for (attr = response->attrs; attr; attr = attr->next) {
        PrinterInfo *pi;

        while (attr != NULL && attr->group_tag != IPP_TAG_PRINTER)
            attr = attr->next;

        if (!attr)
            break;

        pi = g_new0 (PrinterInfo, 1);

        for (; attr && attr->group_tag == IPP_TAG_PRINTER; attr = attr->next) {
            if (!strncmp (attr->name, "device-uri", 10) && attr->value_tag == IPP_TAG_URI) {
                pi->uri = g_strdup (attr->values[0].string.text);
            } else if (!strncmp (attr->name, "printer-name", 12) && attr->value_tag == IPP_TAG_NAME) {
                pi->name = g_strdup (attr->values[0].string.text);
            } 
        }

        if (!pi->uri || !pi->name) {
            free_printer_info (pi, NULL);
        } else {
            *list = g_slist_append (*list, pi);
            dbg ("found cups printer '%s' - '%s'\n", pi->uri, pi->name);
        }
        
        if (!attr)
            break;
    }

    ippDelete (response);
    return TRUE;
}

/*
 * Add and enable a new print queue.
 */
static gboolean add_print_queue (PrinterInfo *pi, const gchar *ppd_file, const gchar *printer_name)
{
    ipp_t *request, *response;
    const gchar *policy;
    gboolean ret = FALSE;
	gchar local_uri [HTTP_MAX_URI + 1];
    
    request = ippNewRequest (CUPS_ADD_PRINTER);
    g_snprintf (local_uri, sizeof local_uri - 1,
		        "ipp://localhost/printers/%s", printer_name);
	ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		          "printer-uri", NULL, local_uri);
	ippAddString (request,	IPP_TAG_PRINTER, IPP_TAG_NAME,
		          "printer-name", NULL, g_strdup (printer_name));
	ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
		          "ppd-name", NULL, g_strdup (ppd_file));
	ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_URI,
		          "device-uri", NULL, pi->uri);
	ippAddBoolean (request, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);
	ippAddInteger(request, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state",
                  IPP_PRINTER_IDLE);

    policy = g_getenv ("CUPS_AUTOCONFIG_POLICY");
    if (policy && strcmp (policy, "")) {
        ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                      "printer-op-policy", NULL, g_strdup (policy));
    }

    response = cupsDoRequest (global_cups_connection, request, "/");
    if (!response || response->request.status.status_code > IPP_OK_CONFLICT) {
        err ("Failed to add new printer queue\n");
        goto done;
    }
    
    ret = TRUE;

done:
    ippDelete (response);
	return ret;
}

/*
 * Enable or disable a printer.
 */
static gboolean set_printer_status (const gchar *printer_name, gboolean enable)
{
    ipp_t *request, *response;
    gboolean ret = FALSE;
	gchar local_uri [HTTP_MAX_URI + 1];
    
    request = enable ? ippNewRequest (IPP_RESUME_PRINTER) :
                       ippNewRequest (IPP_PAUSE_PRINTER);
    
    g_snprintf (local_uri, sizeof local_uri - 1,
		        "ipp://localhost/printers/%s", printer_name);
	ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		          "printer-uri", NULL, local_uri);

    response = cupsDoRequest (global_cups_connection, request, "/");
    if (!response || response->request.status.status_code > IPP_OK_CONFLICT) {
        err ("Failed to change printer state\n");
        goto done;
    }

    ret = TRUE;

done:
    ippDelete (response);
    return ret;
}

/*
 * Get printer information from HAL, cups, and the cups backends
 * and add the new printer, if necessary.
 */
static gboolean printer_added (void)
{
    const gchar *enabled = NULL;
    GSList *detected = NULL, *configured = NULL, *d = NULL, *c = NULL;
    PrinterInfo *new_printer = NULL, *old_printer = NULL;
    gboolean ret = FALSE;
    
    enabled = g_getenv ("CUPS_AUTOCONFIG_ENABLE");
    if (!enabled || strncmp ("yes", enabled, 3)) {
        g_print ("skipping, CUPS_AUTOCONFIG_ENABLE is not 'yes' it's '%s'\n", enabled);
        return TRUE;
    }

    get_detected_printers (&detected);
    if (!detected) {
        err ("Failed to detect new printers\n");
        goto done;
    }

    /* find the printer that HAL says was just added */
    for (d = detected; d; d = d->next) {
        if (printer_matches_hal_properties (d->data)) {
            new_printer = d->data;
            break;
        }
    }
    
    if (!new_printer) {
        err ("Failed to find a printer that matches HAL properties\n");
        goto done;
    }

    get_cups_printers (&configured);
    
    /* see if cups already knows about this printer */
    for (c = configured; c; c = c->next) {
        PrinterInfo *tp = c->data;
        if (!strcmp (tp->uri, new_printer->uri)) {
            old_printer = tp;
            break;
        }
    }
    
    if (!old_printer) {
        gchar *ppd = get_best_ppd (new_printer);
        if (ppd) {
            gchar *name;

            dbg ("selected ppd file is '%s'\n", ppd);
            name = generate_printer_name (new_printer, configured);
            if (add_print_queue (new_printer, ppd, name)) {
                gchar *msg = g_strdup_printf (_("%s has been configured."), name);
                send_notification (_("New Printer"), msg);
                g_free (msg);
            } else {
                err ("Failed to add print queue\n");
                g_free (ppd);
                g_free (name);
                goto done;
            }

            g_free (ppd);
            g_free (name);
        } else {
            err ("Failed to find PPD file for printer\n");
            goto done;
        }
    } else {
        dbg ("Enabling old printer '%s'\n", old_printer->name);
        set_printer_status (old_printer->name, TRUE);
    }

    ret = TRUE;

done:
    g_slist_foreach (detected, free_printer_info, NULL);
    g_slist_free (detected);
    g_slist_foreach (configured, free_printer_info, NULL);
    g_slist_free (configured);
    return ret;
}

/*
 * Look at the list of detected printers and the list of
 * cups configured printers and disable the print queues
 * for the printers that aren't present.
 */
static gboolean printer_removed (void)
{
    const gchar *enabled;
    GSList *detected = NULL, *configured = NULL, *c = NULL;
    gboolean ret = TRUE;
    
    enabled = g_getenv ("CUPS_AUTOCONFIG_DISABLE");
    if (!enabled || strncmp ("yes", enabled, 3)) {
        g_print ("skipping, CUPS_AUTOCONFIG_DISABLE is not 'yes'\n");
        return TRUE;
    }
    
    get_cups_printers (&configured);
    if (!configured) {
        g_print ("There are no configured printers, nothing to do\n");
        return TRUE;
    }

    get_detected_printers (&detected);
    
    for (c = configured; c; c = c->next) {
        GSList *d = NULL;
        gboolean found = FALSE;
        PrinterInfo *pi = c->data;

        if (!uri_is_local (pi->uri))
            continue;

        for (d = detected; d; d = d->next) {
            PrinterInfo *dp = d->data;
            if (!strcmp (dp->uri, pi->uri)) {
                found = TRUE;
                break;
            }
        }
        
        if (!found) {
            ret = set_printer_status (pi->name, FALSE);
            if (ret) {
                gchar *m = g_strdup_printf (_("%s has been disabled."), pi->name);
                send_notification (_("Printer Disabled"), m);
                g_free (m);
            }
        }
    }

    g_slist_foreach (detected, free_printer_info, NULL);
    g_slist_free (detected);
    g_slist_foreach (configured, free_printer_info, NULL);
    g_slist_free (configured);
    return ret;
}

/*
 * This is where all the magic happens.
 */
int main (int argc, char *argv[]) 
{
    GOptionContext *ctx;
    GError *err = NULL;
    gboolean add_cmd = FALSE, remove_cmd = FALSE;
    gboolean ret = FALSE;

    GOptionEntry entries[] = {
        { "add", 0, 0, G_OPTION_ARG_NONE, &add_cmd, "Add new printers", NULL },
        { "remove", 0, 0, G_OPTION_ARG_NONE, &remove_cmd, 
          "Disable printers that aren't connected", NULL },
        { "debug", 0, 0, G_OPTION_ARG_NONE, &enable_debug, "Enable debugging output", NULL },
        { NULL, 0, 0, 0, NULL, NULL, NULL }
    };

    //TODO gettext

    ctx = g_option_context_new ("command_flag");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
        g_printerr ("parsing failed: %s\n", err->message);
        g_error_free (err);
        goto done;
    }

    if (!open_log ())
        goto done;
    
    if (geteuid () != 0) {
        err ("You must be root to run %s\n", argv[0]);
        goto done;
    }

    if (g_getenv ("CUPS_AUTO_CONFIG_DEBUG"))
        enable_debug = TRUE;

    if (!cups_connect ()) {
        err ("Failed to connect to CUPS\n");
        goto done;
    }

    load_vendor_mappings ();

    if (add_cmd) {
        if (printer_added ())
            ret = TRUE;
        else {
            const gchar *udi = g_getenv ("HAL_PROP_INFO_UDI");
            ret = udi ? request_manual_configuration (udi) : FALSE;
        }
    } else if (remove_cmd) {
        ret = printer_removed ();
    } else {
        err ("One, and only one, of --add and --remove must be given\n");
        ret = FALSE;
    }

done:
    cups_disconnect ();
    g_option_context_free (ctx);
    fclose (log_file);
    return ret;
}
