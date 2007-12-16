/*
 * Copyright (c) 2007 Novell, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or 
 * modify it under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, contact Novell, Inc.
 *
 * To contact Novell about this file by physical or electronic mail, 
 * you may find current contact information at www.novell.com.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <config.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <cups/cups.h>
#include <cups/http.h>
#include <cups/ipp.h>
#include <dbus/dbus.h>
#include <hal/libhal.h>

#define LPIOC_GET_DEVICE_ID(len) _IOC(_IOC_READ, 'P', 1, len)

#define CUPS_BACKEND_DIR LIBDIR "/cups/backend"
#define LOGFILE LOCALSTATEDIR "/log/cups-autoconfig.log"
#define CONFIGFILE SYSCONFDIR "/cups-autoconfig.conf"
#define MAX_LOG_SIZE 20971520

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

typedef struct _ConfigInfo {
    gchar *default_policy;
    gboolean add;
    gboolean remove;
} ConfigInfo;

static FILE *log_file;
static ConfigInfo *config;
static GHashTable *alias_map;
static GHashTable *vendor_map;
static http_t *global_cups_connection;
static LibHalContext *hal_ctx;

static void log_it (const char *fmt, ...)
{
    va_list args;
    
    va_start (args, fmt);
    g_vfprintf (log_file, fmt, args);
    va_end (args);

    va_start (args, fmt);
    g_vfprintf (stderr, fmt, args);
    va_end (args);
}

static gboolean load_config (void)
{
    GError *error = NULL;
    gchar *value;
    GKeyFile *kf;

    kf = g_key_file_new ();
    config = g_malloc (sizeof (ConfigInfo));

    if (!g_key_file_load_from_file (kf, CONFIGFILE, G_KEY_FILE_NONE, &error)) {
        log_it ("Error loading config file: %s\n", error->message);
        g_error_free (error);
        return FALSE;
    }

    value = g_key_file_get_value (kf, "CUPS", "ConfigureNewPrinters", NULL);
    config->add = value && (!strcmp (value, "yes") || !strcmp (value, "y")) ? TRUE : FALSE; 
    g_free (value);

    /*value = g_key_file_get_value (kf, "CUPS", "DisablePrintersOnRemoval", NULL);
    config->remove = value && (!strcmp (value, "yes") || !strcmp (value, "y")) ? TRUE : FALSE; 
    g_free (value);*/

    config->remove = FALSE;
    
    value = g_key_file_get_value (kf, "CUPS", "DefaultCUPSPolicy", NULL);
    if (!strcmp ("", value)) {
        g_free (value);
        config->default_policy = NULL;
    } else { 
        config->default_policy = value;
    }

    g_key_file_free (kf);
    return TRUE;
}

static void free_config (void)
{
    g_free (config->default_policy);
    g_free (config);
    config = NULL;
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
    struct stat info;
    gboolean append = TRUE;

    if (!stat (LOGFILE, &info) && info.st_size > MAX_LOG_SIZE)
        append = FALSE;

    log_file = fopen (LOGFILE, append ? "a" : "w");
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
        log_it ("Failed to connect to cupsd\n");
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
 * Set the printer.configured property on new printers for policy
 * applications like gvm.
 */
static void set_printer_configured_existing_property (const gchar *udi, const gchar *name)
{
    if (!libhal_device_set_property_bool (hal_ctx, udi,
                                          "printer.configured_existing", TRUE, NULL))
        log_it ("Failed to set printer.configured_existing property for '%s'\n", udi);
}

/*
 * Set the printer.configured property on new printers for policy
 * applications like gvm.
 */
static void set_printer_configured_property (const gchar *udi, const gchar *name)
{
    if (!libhal_device_set_property_bool (hal_ctx, udi,
                                          "printer.configured", TRUE, NULL))
        log_it ("Failed to set printer.configured property for '%s'\n", udi);

    if (!libhal_device_set_property_string (hal_ctx, udi,
                                            "printer.display_name", name, NULL))
        log_it ("Failed to set printer.display_name\n");
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
 * A crappy check to see if a printer uri is local.
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
 * Extract the vendor, model, and serial number from an ieee 1284 id.
 * The returned strings need to be free by the caller.
 */
static void get_1284_fields (const gchar *id, gchar **vendor, gchar **model, gchar **serial)
{
    gchar *uid = g_ascii_strup (id, -1);

    if (vendor) {
        gchar *s, *e;

        if ((s = strstr (uid, "MFG:")))
            s += 4;
        else if ((s = strstr (uid, "MANUFACTURER:")))
            s += 13;

        if (s && (e = strchr (s, ';'))) {
            *e = '\0';
            *vendor = g_strstrip (g_strdup (s));
        }
    }

    if (model) {
        gchar *s, *e;
        
        if ((s = strstr (uid, "MDL:")))
            s += 4;
        else if ((s = strstr (uid, "MODEL:")))
            s += 6;

        if (s && (e = strchr (s, ';'))) {
            *e = '\0';
            *model = g_strstrip (g_strdup (s));
        }
    }

    if (serial) {
        gchar *s, *e;
        
        if ((s = strstr (uid, "SERN:")))
            s += 5;
        else if ((s = strstr (uid, "SERIALNUMBER:")))
            s += 13; 
        else if ((s = strstr (uid, ";SN:")))
            s += 4;

        if (s && (e = strchr (s, ';'))) {
            *e = '\0';
            *serial = g_strstrip (g_strdup (s));
        }
    }
}

/*
 *  Determines if two printers are the same based on their IEEE 1284 ids.
 */
static gboolean match_by_1284 (const char *id1, const char *id2)
{
    gboolean ret = FALSE;
    gchar *man1 = NULL, *man2 = NULL, *mod1 = NULL;
    gchar *mod2 = NULL, *ser1 = NULL, *ser2 = NULL; 

    get_1284_fields (id1, &man1, &mod1, &ser1);
    get_1284_fields (id2, &man2, &mod2, &ser2);

    if (man1 && man2 && g_ascii_strcasecmp (man1, man2))
        goto done;

    if (mod1 && mod2 && g_ascii_strcasecmp (mod1, mod2))
        goto done;

    if (ser1 && ser2 && g_ascii_strcasecmp (ser1, ser2))
        goto done;

    ret = TRUE;

done:
    g_free (man1);
    g_free (man2);
    g_free (mod1);
    g_free (mod2);
    g_free (ser1);
    g_free (ser2);
    return ret;
}

/* 
 * See if the given printer (pi) matches a HAL printer.  If a hal_udi isn't specified
 * Then we assume we are being called via a HAL callout and we use the environment 
 * variables exposed to the HAL callouts. 
 */
static gboolean printer_matches_hal_properties (PrinterInfo *pi, gchar *hal_udi)
{
    gchar *make = NULL, *model = NULL, *serial = NULL;
    gchar *mm = NULL, *um = NULL, *mdl = NULL;
    gboolean ret = FALSE;

    make = libhal_device_get_property_string (hal_ctx, hal_udi, "printer.vendor", NULL);
    model = libhal_device_get_property_string (hal_ctx, hal_udi, "printer.product", NULL);
    serial = libhal_device_get_property_string (hal_ctx, hal_udi, "printer.serial", NULL);

    log_it ("HAL Printer properties make='%s' model='%s' serial='%s'\n"
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
            log_it ("couldn't find a serial number in the backend uri\n");
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
        log_it ("models '%s' and '%s' didn't match with make '%s'\n",
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
    libhal_free_string (make);
    libhal_free_string (model);
    libhal_free_string (serial);
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
    request = ippNewRequest (CUPS_GET_PPDS);
    response = cupsDoRequest (global_cups_connection, request, "/");
    if (!response || response->request.status.status_code > IPP_OK_CONFLICT) {
        log_it ("Failed to get ppds for '%s'\n", pi->make);
        goto done;
    } 

    /* 
     * Look for ppds that match our model with priority: 
     * manufacturer-PPD > recommended > simple match
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
            log_it ("Matching with 1284 ids '%s' and '%s'\n", pi->device_id, id);
            get_1284_fields (pi->device_id, NULL, &pm, NULL);
            get_1284_fields (id, NULL, &ppd_model, NULL);
            log_it ("Extracted models are '%s' and '%s'\n", pm, ppd_model);
            if (!ppd_model || !pm)
                continue;

            match = !g_ascii_strcasecmp (ppd_model, pm) ? TRUE : FALSE;
            log_it ("Result for matching '%s' and '%s' was %d\n\n", ppd_model, pm, match);
        } else {
            /* match with model strings */
            log_it ("Matching with model strings '%s' and '%s'\n", pi->model, make_and_model);
            ppd_model = model_from_string (pi->make, make_and_model);
            log_it ("Extracted model string from ppd was '%s'\n", ppd_model);
            if (!ppd_model)
                continue;

            match = !g_ascii_strcasecmp (ppd_model, pi->model) ? TRUE : FALSE;
            log_it ("Result for matching '%s' and '%s' was %d\n\n", ppd_model, pi->model, match);
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
        log_it ("%s\n", err->message);
        g_error_free (err);
        g_free (path);
        return FALSE;
    }

    g_free (path);
    fp = fdopen (std_out, "r");
    if (!fp) {
        log_it ("%s\n", strerror (errno));
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

        /* get the uri */
        end = strstr (start, " ");
        if (!end)
            continue;

        *end = '\0';

        /* make sure it's a valid uri for this backend */
        uri = g_strconcat (backend, ":/", NULL);
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
        log_it ("local printer '%s' - '%s'\n", pi->uri, pi->make_and_model);
    }

    fclose (fp);

    if (waitpid (child, &status, 0) == -1 ||
        WEXITSTATUS (status) != 0)
        ret = FALSE;

    g_spawn_close_pid (child);
    return ret;
}

/*
 * See if a usb backend printer is detected by 'backend' and, if so, set 'match'.
 */
static gboolean has_preferred_backend_match (PrinterInfo *usbp, PrinterInfo **match,
                                               const gchar *backend)
{
    GSList *detected = NULL, *p;

    if (!get_local_printers (&detected, backend)) {
        log_it ("Failed to list printers from '%s' backend\n", backend);
        return FALSE;
    }

    /*
     * The flow of this is funny because we want to free all nodes
     * except the one that matches.
     */
    for (p = detected; p; p = p->next) {
        PrinterInfo *sp = p->data;
        gchar *s1, *s2;

        if (*match) {
            free_printer_info (sp, NULL);
            continue;
        }

        if (usbp->device_id && sp->device_id) {
            /* match by ieee1284 ids */
            if (match_by_1284 (sp->device_id, usbp->device_id)) {
                *match = sp;
                continue;
            }
        } else {
            /* match by make and model */
            if (g_ascii_strcasecmp (usbp->make_and_model, sp->make_and_model)) {
                free_printer_info (sp, NULL);
                continue;
            }

            s1 = strstr (usbp->uri, "?serial=");
            s2 = strstr (sp->uri, "?serial=");

            if (s1)
                s1 += 8;
            if (s2)
                s2 += 8;
           
            /* check the serial numbers if both printers have them */
            if (s1 && s2) {
                if (!g_ascii_strcasecmp (s1, s2)) {
                    *match = sp;
                    continue;
                }
            } else {
                *match = sp;
                continue;
            }
        }

        free_printer_info (sp, NULL);
    }

    g_slist_free (detected);
    return *match ? TRUE : FALSE;
}

/*
 * Get the printers that the cups backends detects.
 */
static gboolean get_detected_printers (GSList **list)
{
    GSList *ret = NULL, *p;
    const char *pref_list[] = { "hp", "epson", "canon" };
    int i;

    if (!get_local_printers (&ret, "usb")) {
        log_it ("Failed to get printers from usb backend\n");
        return FALSE;
    }

    /* 
     * See if the detected usb printers match one of printers
     * detected by the preferred backends.
     */
    for (p = ret; p; p = p->next) {
        PrinterInfo *match = NULL, *pi = p->data;

        for (i = 0; i < sizeof(pref_list) / sizeof(pref_list[0]); i++) {
            if (has_preferred_backend_match (pi, &match, pref_list[i])) {
                log_it ("preferring '%s' over '%s'\n", match->uri, pi->uri);
                free_printer_info (pi, NULL);
                p->data = match;
                break;
            }
        }
    }

    *list = ret;
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
        log_it ("Failed to get the list of printers from cupsd\n");
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
            } else if (!strcmp ("printer-make-and-model", attr->name)) {
                pi->make_and_model = g_strdup (attr->values[0].string.text);
            } 
        }

        if (!pi->uri || !pi->name) {
            free_printer_info (pi, NULL);
        } else {
            *list = g_slist_append (*list, pi);
            log_it ("CUPS printer '%s' - '%s'\n", pi->uri, pi->name);
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
static gboolean add_print_queue (const gchar *uri, const gchar *ppd_file, const gchar *printer_name)
{
    ipp_t *request, *response;
    gboolean ret = FALSE;
	gchar local_uri [HTTP_MAX_URI + 1];
  
    log_it ("adding queue with uri='%s' ppd='%s' name='%s'\n",
         uri, ppd_file, printer_name);
    g_snprintf (local_uri, sizeof local_uri - 1,
		        "ipp://localhost/printers/%s", printer_name);
    
    request = ippNewRequest (CUPS_ADD_MODIFY_PRINTER);
	ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		          "printer-uri", NULL, local_uri);
	ippAddString (request,	IPP_TAG_PRINTER, IPP_TAG_NAME,
		          "printer-name", NULL, g_strdup (printer_name));
	ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
		          "ppd-name", NULL, g_strdup (ppd_file));
	ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_URI,
		          "device-uri", NULL, uri);
	ippAddBoolean (request, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);
	ippAddInteger(request, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state",
                  IPP_PRINTER_IDLE);

    if (config->default_policy)
        ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_NAME,
                      "printer-op-policy", NULL, g_strdup (config->default_policy));

    response = cupsDoRequest (global_cups_connection, request, "/");
    if (!response || response->request.status.status_code > IPP_OK_CONFLICT) {
        log_it ("Failed to add new printer queue\n");
        goto done;
    }
    
    ret = TRUE;

done:
    ippDelete (response);
	return ret;
}

/*
 * Remove a printer queue.
 */
static gboolean remove_print_queue (const char *printer_name)
{
    ipp_t *request, *response;
    gboolean ret = FALSE;
	gchar local_uri [HTTP_MAX_URI + 1];
    
    request = ippNewRequest (CUPS_DELETE_PRINTER);
    
    g_snprintf (local_uri, sizeof local_uri - 1,
		        "ipp://localhost/printers/%s", printer_name);
	ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		          "printer-uri", NULL, local_uri);

    log_it ("attempting to remove '%s'\n", local_uri);
    response = cupsDoRequest (global_cups_connection, request, "/");
    if (!response || response->request.status.status_code > IPP_OK_CONFLICT) {
        log_it ("Failed to remove printer\n");
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
        log_it ("Failed to change printer state\n");
        goto done;
    }

    ret = TRUE;

done:
    ippDelete (response);
    return ret;
}

/*
 * Get the IEEE 1284 id from a printer device.  If the
 * return value is non-NULL then it needs to be freed by
 * the caller.
 */
static gchar *get_1284_id_from_device (const char *device)
{
    int fd;
    unsigned int length;
    gchar buff[1024];

    fd = open (device, O_RDWR);
    if (fd == -1) {
        log_it ("open failed: %s\n", strerror (errno));
        return NULL;
    }

    if (ioctl (fd, LPIOC_GET_DEVICE_ID(1024), buff)) {
        log_it ("ioctl failed: %s\n", strerror (errno));
        close (fd);
        return NULL;
    }

    length = (((unsigned) buff[0] & 255) << 8) + ((unsigned) buff[1] & 255);
    buff[length] = '\0';
    close (fd);
    return g_strdup (buff + 2);
}

/*
 * Find the corresponding usb backend printer for the given hal
 * printer.  The returned string needs to be freed by the caller.
 */
static gchar *hal_to_usb_uri (PrinterInfo *hp)
{
    GSList *detected = NULL, *d = NULL;
    gchar *ret = NULL;

    get_detected_printers (&detected);
    if (!detected) {
        log_it ("There are no local printers detected\n");
        return NULL;
    }

    for (d = detected; d; d = d->next) {
        PrinterInfo *pi = d->data;
        
        /* use IEEE 1284 ids to match if we can */
        if (pi->device_id) {
            gchar *dev_file = libhal_device_get_property_string (hal_ctx, hp->uri + 6,
                                                                 "linux.device_file", NULL);
            if (dev_file) {
                gchar *ieee_id = get_1284_id_from_device (dev_file);
                g_free (dev_file);

                if (ieee_id) {
                    gboolean match = match_by_1284 (pi->device_id, ieee_id);
                    
                    log_it ("Trying to match 1284 ids '%s' and '%s'\n", pi->device_id, ieee_id);
                    g_free (ieee_id);
                    
                    if (match) {
                        log_it ("1284 ids matched for '%s' and '%s'\n", pi->uri, hp->uri);
                        ret = g_strdup (pi->uri);
                        goto done;
                    } else {
                        log_it ("1284 ids didn't match\n");
                        continue;
                    }
                }
            }
        }

        /* no 1284 id so we have to use string matching */
        log_it ("no 1284 ids, using string matching\n");
        if (printer_matches_hal_properties (pi, hp->uri + 6)) {
            log_it ("strings matched hal uri '%s'\n", hp->uri);
            ret = g_strdup (pi->uri);
            break;
        } 
    }

done:
    g_slist_foreach (detected, free_printer_info, NULL);
    g_slist_free (detected);
    return ret;
}

/*
 * Find matching ppd-name given a ppd make and model
 */
static gchar *find_matching_ppd (const gchar *ppd_make_and_model)
{
    gchar *p, *ppd = NULL;
    gchar *mm = g_strdup (ppd_make_and_model);
    ipp_t *request, *response;
    ipp_attribute_t *attr;

    /*
     * The printer-make-and-model returned by CUPS might contain this crap 
     */
    p = strstr (mm, " (recommended)");
    if (p)
        *p = '\0';

    request = ippNewRequest (CUPS_GET_PPDS);
    response = cupsDoRequest (global_cups_connection, request, "/");
    if (!response || response->request.status.status_code > IPP_OK_CONFLICT) {
        log_it ("Failed to get ppds\n");
        goto done;
    }
 
    for (attr = response->attrs; attr; attr = attr ? attr->next : NULL) {
        gchar *name = NULL, *make_and_model = NULL;
        
        while (attr && attr->group_tag != IPP_TAG_PRINTER)
            attr = attr->next;

        if (!attr)
            goto done;

        for (; attr && attr->group_tag == IPP_TAG_PRINTER; attr = attr->next) {
            if (!strcmp (attr->name, "ppd-name") && attr->value_tag == IPP_TAG_NAME) {
                name = attr->values[0].string.text;
            } else if (!strcmp (attr->name, "ppd-make-and-model") && attr->value_tag == IPP_TAG_TEXT) {
                make_and_model = attr->values[0].string.text;
                p = strstr (make_and_model, " (recommended)");
                if (p)
                    *p = '\0';
            } 
        }

        log_it ("find_matching_ppd: comparing '%s' and '%s'\n", mm, make_and_model);
        if (!g_ascii_strcasecmp (mm, make_and_model)) {
            ppd = g_strdup (name);
            break;
        }
    }

done:
    g_free (mm);
    ippDelete (response);
    return ppd;
}

/*
 * Move all hal backend printers to the usb backend.
 */
static gboolean migrate_hal_printers (void)
{
    GSList *configured = NULL, *c = NULL;
    gboolean ret = FALSE;

    get_cups_printers (&configured);
    if (!configured)
        return TRUE;
    
    for (c = configured; c; c = c->next) {
        gchar *ppd_file, *usb_uri;
        PrinterInfo *tmp = c->data;

        if (strncmp (tmp->uri, "hal://", 6))
            continue;

        usb_uri = hal_to_usb_uri (tmp);
        if (!usb_uri)
            continue;

        log_it ("hal uri '%s' matched uri '%s'\n", tmp->uri, usb_uri);
        ppd_file = find_matching_ppd (tmp->make_and_model);
        if (!ppd_file) {
            log_it ("Failed to find matching ppd for '%s' '%s'\n", tmp->uri, tmp->make_and_model);
            goto done;
        }
            
        log_it ("Found matching ppd '%s'\n", ppd_file);
        if (!remove_print_queue (tmp->name)) {
            log_it ("Failed to remove hal print queue\n");
        } else {
            if (!add_print_queue (usb_uri, ppd_file, tmp->name)) {
                log_it ("Failed to add usb print queue\n");
            }
        }

        g_free (usb_uri);
        g_free (ppd_file);
    }

    ret = TRUE;

done:
    g_slist_foreach (configured, free_printer_info, NULL);
    g_slist_free (configured);
    return ret;
}

/*
 * Get printer information from HAL, cups, and the cups backends
 * and add the new printer, if necessary.
 */
static gboolean add_printers (void)
{
    const gchar *udi = g_getenv ("HAL_PROP_INFO_UDI");
    char **printers;
    GSList *detected = NULL, *configured = NULL, *d = NULL, *c = NULL;
    gboolean ret = FALSE;
    int i, n;
    
    if (!config->add) {
        g_print ("skipping, CUPS_AUTOCONFIG_ENABLE is not yes\n");
        return TRUE;
    }

    get_cups_printers (&configured);
    get_detected_printers (&detected);
    if (!detected) {
        log_it ("Failed to detect backend printers\n");
        goto done;
    }

    if (!udi) {
        printers = libhal_find_device_by_capability (hal_ctx, "printer", &n, NULL);
    } else {
        printers = g_malloc (sizeof (char *));
        printers[0] = g_strdup (udi); 
        n = 1;
    }
   
    for (i = 0; i < n; i++) {
        PrinterInfo *new_printer = NULL, *old_printer = NULL;
        gchar *ppd, *name;

        /* see if the detected printer matches our hal printer */
        for (d = detected; d; d = d->next) {
            if (printer_matches_hal_properties (d->data, printers[i])) {
                new_printer = d->data;
                break;
            } 
        }
       
        if (!new_printer) {
            log_it ("Failed to find a printer that matches HAL properties\n");
            continue;
        }

        /* see if the printer is configured already */
        for (c = configured; c; c = c->next) {
            PrinterInfo *tp = c->data;
            if (!strcmp (tp->uri, new_printer->uri)) {
                old_printer = tp;
                break;
            }
        }

        /* make sure this printer is enabled */
        if (old_printer) {
            log_it ("Enabling old printer '%s'\n", old_printer->name);
            set_printer_status (old_printer->name, TRUE);
            set_printer_configured_existing_property (printers[i], name);
            continue;
        }
       
        /* try and find a ppd for the new printer */
        ppd = get_best_ppd (new_printer);
        if (!ppd) {
            log_it ("Failed to find PPD file for printer\n");
            continue;
        }

        log_it ("selected ppd file is '%s'\n", ppd);
        name = generate_printer_name (new_printer, configured);
        if (add_print_queue (new_printer->uri, ppd, name)) {
            set_printer_configured_property (printers[i], name);
        } else {
            log_it ("Failed to add print queue\n");
        }

        g_free (ppd);
        g_free (name);
    }

    if (!udi) {
        libhal_free_string_array (printers);
    } else {
        g_free (printers[0]);
        g_free (printers);
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
static gboolean disable_printers (void)
{
    GSList *detected = NULL, *configured = NULL, *c = NULL;
    gboolean ret = TRUE;
    
    if (!config->remove) {
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
        
        if (found)
            continue;

        if (!set_printer_status (pi->name, FALSE))
            ret = FALSE;
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
    GOptionContext *ctx = NULL;
    GError *err = NULL;
    DBusError error;
    gboolean add_cmd = FALSE, disable_cmd = FALSE;
    gboolean ret = FALSE, is_add_enabled = FALSE, migrate = FALSE;

    GOptionEntry entries[] = {
        { "add", 0, 0, G_OPTION_ARG_NONE, &add_cmd, "Add new printers", NULL },
        { "disable", 0, 0, G_OPTION_ARG_NONE, &disable_cmd, 
          "Disable printers that aren't connected", NULL },
        { "migrate-hal-printers", 0, 0, G_OPTION_ARG_NONE, &migrate, "Migrate HAL backend printers", NULL },
        { "is-add-enabled", 0, 0, G_OPTION_ARG_NONE, &is_add_enabled,
          "Check if the ConfigureNewPrinters option is set", NULL },
        { NULL, 0, 0, 0, NULL, NULL, NULL }
    };

    bindtextdomain (GETTEXT_PACKAGE, NULL);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    if (!open_log ())
        goto done;
    
    ctx = g_option_context_new ("");
    g_option_context_add_main_entries (ctx, entries, NULL);
    if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
        log_it ("parsing failed: %s\n", err->message);
        g_error_free (err);
        goto done;
    }

    if (geteuid () != 0) {
        log_it ("You must be root to run %s\n", argv[0]);
        goto done;
    }

    if (!load_config ()) {
        log_it ("Failed to load config file\n");
        goto done;
    }
    
    load_vendor_mappings ();
    
    if (!cups_connect ()) {
        log_it ("Failed to connect to CUPS\n");
        goto done;
    }

    if (!(hal_ctx = libhal_ctx_new ())) {
        log_it ("Unable to create HAL context\n");
        goto done;
    }
	
    dbus_error_init (&error);
    libhal_ctx_set_dbus_connection (hal_ctx, dbus_bus_get (DBUS_BUS_SYSTEM, &error));
	
    if (!libhal_ctx_init (hal_ctx, &error)) {
        log_it ("Unable to init HAL context: %s\n", error.message);
        dbus_error_free (&error);
        goto done;
    }

    if (migrate && !migrate_hal_printers ())
        log_it ("Failed to migrate hal printers\n");

    if (add_cmd) {
        if (add_printers ())
            ret = TRUE;
    } else if (disable_cmd) {
        ret = disable_printers ();
    } else if (is_add_enabled) {
        ret = config->add;
    } else if (migrate) {
        /* we're only doing migration */
    } else {
        /* just show the help */
        gchar *help_txt = g_option_context_get_help (ctx, FALSE, NULL);
        g_printerr ("%s\n", help_txt);
        g_free (help_txt);
    }

done:
    if (hal_ctx) {
        libhal_ctx_shutdown (hal_ctx, NULL);
        libhal_ctx_free (hal_ctx);
    }
    
    cups_disconnect ();

    if (ctx)
        g_option_context_free (ctx);

    if (config)
        free_config ();
    
    if (log_file)
        fclose (log_file);
    
    return ret ? 0 : 1;
}
