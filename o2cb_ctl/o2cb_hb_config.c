/* -*- mode: c; c-basic-offset: 4; -*-
 *  vim:expandtab:shiftwidth=4:tabstop=4:
 *
 * o2cb_hb_config.c
 *
 * Configuration program for O2CB heartbeat.
 *
 * Copyright (C) 2005 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License, version 2,  as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include <glib.h>

#include "jiterator.h"
#include "jconfig.h"

#define PROGNAME "o2cb_hb_config"
#define HB_CONFIG_FILE "/etc/ocfs2/heartbeat.conf"


typedef enum {
    HBCONF_OP_NONE,
    HBCONF_OP_INFO,
    HBCONF_OP_ADD,
    HBCONF_OP_REMOVE,
    HBCONF_OP_MODE,
} HBConfOperation;

typedef enum {
    HBCONF_PRINT_READABLE = 0,
    HBCONF_PRINT_PARSEABLE,
} HBConfPrintMode;

typedef struct _HBConfContext HBConfContext;

struct _HBConfContext
{
    JConfig *c_cf;
    gchar *c_cluster;
    gchar *c_set_mode;
    HBConfOperation c_op;
    HBConfPrintMode c_print_mode;
};


static void print_usage(gint rc);


static gint hbconf_config_load(HBConfContext *ctxt,
                               const gchar *filename)
{
    gint rc;
    JConfigCtxt *cf_ctxt;
    JConfig *cf;
    struct stat stat_buf;

    rc = stat(filename, &stat_buf);
    if (rc)
    {
        rc = -errno;
        if (rc != -ENOENT)
            return rc;
        cf = j_config_parse_memory("", strlen(""));
        if (!cf)
            return -ENOMEM;
    }
    else
    {
        cf_ctxt = j_config_new_context();
        if (!cf_ctxt)
            return -ENOMEM;
        j_config_context_set_verbose(cf_ctxt, FALSE);

        cf = j_config_parse_file_with_context(cf_ctxt, filename);
        if (j_config_context_get_error(cf_ctxt))
        {
            if (cf)
            {
                j_config_free(cf);
                cf = NULL;
            }
        }
        j_config_context_free(cf_ctxt);
        
        if (!cf)
            return -EIO;
    }

    ctxt->c_cf = cf;
    return 0;
}

static gint write_file(const gchar *text, const gchar *filename)
{
    int rc, fd;
    GString *template;
    FILE *file;
    size_t written, len;

    rc = mkdir("/etc/ocfs2", 0755);
    if (rc)
    {
        rc = -errno;
        if (rc == -EEXIST)
            rc = 0;
        else
            goto out;
    }

    rc = -ENOMEM;
    template = g_string_new(filename);
    if (!template)
        goto out;

    g_string_append(template, "XXXXXX");
    fd = mkstemp(template->str);
    rc = -errno;
    if (fd < 0)
        goto out_free;

    file = fdopen(fd, "w");
    if (!file)
    {
        rc = -errno;
        close(fd);
        goto out_unlink;
    }

    len = strlen(text);
    written = fwrite(text, sizeof(char), len, file);
    if (written != len)
    {
        if (feof(file))
            rc = -EIO;
        else if (ferror(file))
            rc = -errno;
        else
            rc = -EIO;

        fclose(file);
        goto out_unlink;
    }

    fchmod(fd, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    rc = fclose(file);
    if (rc)
    {
        rc = -errno;
        goto out_unlink;
    }

    rc = rename(template->str, filename);
    if (rc)
        rc = -errno;
    
out_unlink:
    if (rc)
        unlink(template->str);

out_free:
    g_string_free(template, TRUE);

out:
    return rc;
}


static gint hbconf_config_store(HBConfContext *ctxt,
                                const gchar *filename)
{
    int rc;
    char *text;

    rc = -ENOMEM;
    text = j_config_dump_memory(ctxt->c_cf);
    if (!text)
        goto out;

    rc = write_file(text, filename);
    g_free(text);

out:
    return rc;
}

static gint hbconf_mode_show_one(HBConfContext *ctxt,
                                 JConfigStanza *cfs)
{
    gchar *cluster;
    gchar *mode;

    /* Caller has verified this part of the stanza */
    cluster = j_config_get_attribute(cfs, "name");

    mode = j_config_get_attribute(cfs, "mode");
    if (!mode || (strcmp(mode, "local") && strcmp(mode, "global")))
    {
        fprintf(stderr,
                PROGNAME ": Cluster \"%s\" does not have a valid mode.\n",
                cluster);
        return -EINVAL;
    }

    if (ctxt->c_print_mode == HBCONF_PRINT_READABLE)
        fprintf(stdout, "Cluster \"%s\" uses %s heartbeating.\n",
                cluster, mode);
    else if (ctxt->c_print_mode == HBCONF_PRINT_PARSEABLE)
        fprintf(stdout, "%s:%s\n", cluster, mode);

    return 0;
}

static gint hbconf_mode_show(HBConfContext *ctxt)
{
    gint rc;
    JIterator *iter;
    JConfigStanza *cfs;
    JConfigMatch match =
    {
       J_CONFIG_MATCH_VALUE, "name", ctxt->c_cluster
    };

    rc = -ENOMEM;
    if (ctxt->c_cluster)
        iter = j_config_get_stanzas(ctxt->c_cf, "cluster", &match, 1);
    else
        iter = j_config_get_stanzas(ctxt->c_cf, "cluster", NULL, 0);
    if (!iter)
    {
        fprintf(stderr, PROGNAME ": Unable to allocate memory.\n");
        goto out_error;
    }

    rc = -ENOENT;
    if (!j_iterator_has_more(iter))
    {
        if (ctxt->c_cluster)
        {
            fprintf(stderr,
                    PROGNAME ": Cluster \"%s\" does not exist.\n",
                    ctxt->c_cluster);
        }
        else
        {
            fprintf(stderr,
                    PROGNAME ": There are no configured clusters.\n");
        }

        goto out_iter;
    }

    while (j_iterator_has_more(iter))
    {
        cfs = (JConfigStanza *)j_iterator_get_next(iter);
        rc = hbconf_mode_show_one(ctxt, cfs);
        if (rc)
            break;
        
        /* Silently ignore multiple definitions of a specific cluster */
        if (ctxt->c_cluster)
            break;
    }
    
out_iter:
    j_iterator_free(iter);

out_error:
    return rc;
}

static gint hbconf_mode_set(HBConfContext *ctxt)
{
    gint rc;
    JIterator *iter;
    JConfigStanza *cfs = NULL;
    JConfigMatch match =
    {
       J_CONFIG_MATCH_VALUE, "name", ctxt->c_cluster
    };

    if (!ctxt->c_cluster)
    {
        fprintf(stderr,
                PROGNAME ": Cluster not specified.\n");
        print_usage(-EINVAL);
    }

    if (strcmp(ctxt->c_set_mode, "global") &&
        strcmp(ctxt->c_set_mode, "local"))
    {
        fprintf(stderr,
                PROGNAME ": Invalid heartbeat mode: \"%s\"\n",
                ctxt->c_set_mode);
        print_usage(-EINVAL);
    }

    rc = -ENOMEM;
    iter = j_config_get_stanzas(ctxt->c_cf, "cluster", &match, 1);
    if (!iter)
    {
        fprintf(stderr,
                PROGNAME ": Unable to allocate memory.\n");
        goto out;
    }

    if (j_iterator_has_more(iter))
        cfs = (JConfigStanza *)j_iterator_get_next(iter);
    j_iterator_free(iter);

    if (!cfs)
    {
        cfs = j_config_add_stanza(ctxt->c_cf, "cluster");
        if (!cfs)
        {
            fprintf(stderr, PROGNAME ": Unable to allocate memory.\n");
            goto out;
        }
        j_config_set_attribute(cfs, "name", ctxt->c_cluster);
    }

    j_config_set_attribute(cfs, "mode", ctxt->c_set_mode);

    rc = hbconf_config_store(ctxt, HB_CONFIG_FILE);
    if (rc)
    {
        fprintf(stderr,
                PROGNAME ": Error storing \"%s\": %s\n",
                HB_CONFIG_FILE, strerror(-rc));
    }

out:
    return rc;
}

static gint hbconf_mode(HBConfContext *ctxt)
{
    gint rc;

    if (ctxt->c_set_mode)
        rc = hbconf_mode_set(ctxt);
    else
        rc = hbconf_mode_show(ctxt);

    return rc;
}

static void print_usage(gint rc)
{
    FILE *output = rc ? stderr : stdout;

    fprintf(output,
            "Usage: " PROGNAME " \n");

    exit(rc);
}

static void print_version(void)
{
    fprintf(stdout, PROGNAME "version %s\n", VERSION);
    exit(0);
}

extern char *optarg;
extern int optopt;
extern int opterr;
extern int optind;
static gint parse_options(gint argc, gchar *argv[], HBConfContext *ctxt)
{
    int c;
    HBConfOperation op = HBCONF_OP_NONE;

    opterr = 0;
    while ((c = getopt(argc, argv, ":hVARIMozc:m:-:")) != EOF)
    {
        switch (c)
        {
            case 'h':
                print_usage(0);
                break;

            case 'V':
                print_version();
                break;

            case '-':
                if (!strcmp(optarg, "version"))
                    print_version();
                else if (!strcmp(optarg, "help"))
                    print_usage(0);
                else
                {
                    fprintf(stderr, PROGNAME ": Invalid option \'--%s\'\n",
                            optarg);
                    return -EINVAL;
                }
                break;

            case 'A':
                op = HBCONF_OP_ADD;
                break;

            case 'R':
                op = HBCONF_OP_REMOVE;
                break;

            case 'M':
                op = HBCONF_OP_MODE;
                break;

            case 'o':
                ctxt->c_print_mode = HBCONF_PRINT_PARSEABLE;
                break;

            case 'z':
                ctxt->c_print_mode = HBCONF_PRINT_READABLE;
                break;

            case 'c':
                ctxt->c_cluster = optarg;
                break;

            case 'm':
                ctxt->c_set_mode = optarg;
                break;

            default:
                fprintf(stderr,
                        PROGNAME ": Shouldn't get here %c %c\n",
                        optopt, c);
                return -EINVAL;
                break;
        }

        if (op != HBCONF_OP_NONE)
        {
            if (ctxt->c_op != HBCONF_OP_NONE)
            {
                fprintf(stderr,
                        PROGNAME ": Specify only one operation.\n");
                return -EINVAL;
            }

            ctxt->c_op = op;
            op = HBCONF_OP_NONE;
        }
    }

    if ((ctxt->c_op != HBCONF_OP_MODE) && ctxt->c_set_mode)
    {
        fprintf(stderr, PROGNAME ": Option \'-m\' is invalid for this operation.\n");
        return -EINVAL;
    }

    return 0;
}

gint main(gint argc, gchar *argv[])
{
    gint rc;
    HBConfContext ctxt = {0, };

    ctxt.c_print_mode = HBCONF_PRINT_READABLE;
    rc = parse_options(argc, argv, &ctxt);
    if (rc)
        print_usage(rc);

    rc = hbconf_config_load(&ctxt, HB_CONFIG_FILE);
    if (rc)
        goto out;

    switch (ctxt.c_op)
    {
        case HBCONF_OP_MODE:
            rc = hbconf_mode(&ctxt);
            break;

        default:
            fprintf(stderr, PROGNAME ": Can't get here!  op %d\n",
                    ctxt.c_op);
            rc = -EINVAL;
            break;
    }

out:
    return rc;
}