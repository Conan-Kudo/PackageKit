/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Helio Chissini de Castro <helio@kde.org>
 * Copyright (C) 2015 Kevin Kofler <kevin.kofler@chello.at>
 * Copyright (C) 2015 Neal Gompa <ngompa13@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gmodule.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <libhif.h>
#include <appstream-glib.h>

/* allow compiling with older libhif versions */
#if !HIF_CHECK_VERSION(0,2,0)
#include <libhif-private.h>
#define hif_error_set_from_hawkey(r,e)	hif_rc_to_gerror(r,e)
#endif

#include <pk-backend.h>
#include <pk-cleanup.h>
#include <packagekit-glib2/pk-debug.h>

#include <hawkey/advisory.h>
#include <hawkey/advisoryref.h>
#include <hawkey/errno.h>
#include <hawkey/packagelist.h>
#include <hawkey/packageset.h>
#include <hawkey/query.h>
#include <hawkey/stringarray.h>
#include <hawkey/version.h>
#include <hawkey/util.h>
#include <librepo/librepo.h>

#include "hif-backend.h"

typedef struct {
	HySack		 sack;
	gboolean	 valid;
	gchar		*key;
} HifSackCacheItem;

typedef struct {
	HifContext	*context;
	GHashTable	*sack_cache;	/* of HifSackCacheItem */
	GMutex		 sack_mutex;
	HifRepos	*repos;
	GTimer		*repos_timer;
} PkBackendHifPrivate;

typedef struct {
	GPtrArray	*sources;
	HifTransaction	*transaction;
	HifState	*state;
	PkBackend	*backend;
	PkBitfield	 transaction_flags;
	HyGoal		 goal;
} PkBackendHifJobData;

typedef enum {
	COMPS_STATE_CATEGORY,
	COMPS_STATE_CATEGORY_ID,
	COMPS_STATE_CATEGORY_GROUPID,
	COMPS_STATE_GROUP,
	COMPS_STATE_GROUP_ID,
	COMPS_STATE_GROUP_PKGREQ,
	COMPS_STATE_GROUP_DESCRIPTION,
	COMPS_STATE_IGNORE
} PkCompsState;

typedef enum {
	COMPS_QUERY_CATEGORY,
	COMPS_QUERY_GROUP
} PkCompsQuery;

typedef struct {
	gchar *current_query;
	gboolean query_match;
	GPtrArray *groups;
	GPtrArray *packages;
	GPtrArray *comps;
	PkCompsState category_state;
	PkCompsQuery query;
} PkCompsData;

static void
pk_comps_start_element (GMarkupParseContext *context,
		const gchar         *element_name,
		const gchar        **attribute_names,
		const gchar        **attribute_values,
		gpointer             user_data,
		GError             **error)
{

	const gchar* const * current_locale = g_get_language_names();
	PkCompsData *comps_data = (PkCompsData*) (user_data);

	if (comps_data->query == COMPS_QUERY_CATEGORY) {
		if (g_strcmp0 (element_name, "category") == 0)
			comps_data->category_state = COMPS_STATE_CATEGORY;
		if (g_strcmp0 (element_name, "id") == 0  && comps_data->category_state == COMPS_STATE_CATEGORY)
			comps_data->category_state = COMPS_STATE_CATEGORY_ID;
		if (g_strcmp0 (element_name, "groupid") == 0)
			comps_data->category_state = COMPS_STATE_CATEGORY_GROUPID;
		if (g_strcmp0 (element_name, "name") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
		if (g_strcmp0 (element_name, "description") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
		if (g_strcmp0 (element_name, "display_order") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
		if (g_strcmp0 (element_name, "grouplist") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
	}
	if (comps_data->query == COMPS_QUERY_GROUP) {
		if (g_strcmp0 (element_name, "group") == 0)
			comps_data->category_state = COMPS_STATE_GROUP;
		if (g_strcmp0 (element_name, "id") == 0  && comps_data->category_state == COMPS_STATE_GROUP)
			comps_data->category_state = COMPS_STATE_GROUP_ID;
		if (g_strcmp0 (element_name, "packagereq") == 0)
			comps_data->category_state = COMPS_STATE_GROUP_PKGREQ;
		if (g_strcmp0 (element_name, "name") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
		if (g_strcmp0 (element_name, "description") == 0) {
			if (g_strcmp0 (attribute_values[0], current_locale[0]) ==  0) {
				comps_data->category_state = COMPS_STATE_GROUP_DESCRIPTION;
			} else {
				comps_data->category_state = COMPS_STATE_IGNORE;
			}
		}
		if (g_strcmp0 (element_name, "default") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
		if (g_strcmp0 (element_name, "uservisible") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
		if (g_strcmp0 (element_name, "packagelist") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
	}

}

static void
pk_comps_element_text (GMarkupParseContext *context,
    const gchar         *text,
    gsize                text_len,
    gpointer             user_data,
    GError             **error)
{
	PkCompsData *comps_data = (PkCompsData*) (user_data);

	if (comps_data->category_state == COMPS_STATE_CATEGORY_ID &&
	    g_strcmp0 (comps_data->current_query, text) == 0)
		comps_data->query_match = TRUE;

	if (comps_data->category_state == COMPS_STATE_GROUP_ID &&
	    g_strcmp0 (comps_data->current_query, text) == 0)
		comps_data->query_match = TRUE;

	if (comps_data->query_match) {
		if (comps_data->category_state == COMPS_STATE_CATEGORY_GROUPID) {
			g_debug ("Group: %s", text);
			g_ptr_array_add (comps_data->groups, g_strdup(text));
		}
		if (comps_data->category_state == COMPS_STATE_GROUP_DESCRIPTION) {
			g_debug ("Description: %s", text);
		}
		if (comps_data->category_state == COMPS_STATE_GROUP_PKGREQ) {
			g_debug ("Package: %s", text);
			g_ptr_array_add (comps_data->packages, g_strdup(text));
		}
	}
}

static void
pk_comps_end_element (GMarkupParseContext *context,
    const gchar         *element_name,
    gpointer             user_data,
    GError             **error)
{
	PkCompsData *comps_data = (PkCompsData*) (user_data);

	if (comps_data->query_match) {
		if (g_strcmp0 (element_name, "groupid") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
		if (g_strcmp0 (element_name, "packagereq") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
		if (g_strcmp0 (element_name, "description") == 0)
			comps_data->category_state = COMPS_STATE_IGNORE;
		if (g_strcmp0 (element_name, "category") == 0)
			comps_data->query_match = FALSE;
		if (g_strcmp0 (element_name, "group") == 0)
			comps_data->query_match = FALSE;
	}
}

static GMarkupParser comps_parser = {
	pk_comps_start_element,
	pk_comps_end_element,
	pk_comps_element_text,
	NULL,
	NULL
};

/**
 * pk_backend_comps_parser
 */
static gboolean
pk_backend_comps_parser (gpointer user_data)
{
	guint i;
	_cleanup_free_ gchar *text = NULL;
	gsize length;
	GMarkupParseContext *context = NULL;
	PkCompsData *comps_data = (PkCompsData*) (user_data);

	comps_data->category_state = COMPS_STATE_IGNORE;

	context = g_markup_parse_context_new (&comps_parser, 0, comps_data, NULL);

	for (i = 0; i < comps_data->comps->len; i++) {
		g_debug ("Comps file parsed: %s.", (char*) (g_ptr_array_index (comps_data->comps, i)));
		if (!g_file_get_contents (g_ptr_array_index (comps_data->comps, i), &text, &length, NULL)) {
			g_debug ("Couldn't load XML");
			return FALSE;
		} else if (!g_markup_parse_context_parse (context, text, length, NULL)) {
			g_debug ("Parse failed");
			return FALSE;
		}
	}

	g_markup_parse_context_free (context);
	return TRUE;
}

/**
 * pk_backend_get_packages_from_group
 */
static gchar **
pk_backend_get_packages_from_group (gchar **groups,
		gpointer user_data)
{
	guint i = 0;
	PkCompsData *comps_data = (PkCompsData*) (user_data);

	comps_data->query = COMPS_QUERY_GROUP;
	comps_data->packages = g_ptr_array_new_with_free_func (g_free);

	for (i = 0; i < g_strv_length (groups); i++) {
		comps_data->current_query = g_strdup (groups[i]);
		if (!pk_backend_comps_parser (comps_data))
			g_debug ("Group %s not available !", groups[i]);
	}

	g_ptr_array_add (comps_data->packages, NULL);
	return pk_ptr_array_to_strv (comps_data->packages);
}

/**
 * pk_backend_get_groups_from_category
 */
static gchar **
pk_backend_get_groups_from_category (const gchar *category,
		gpointer user_data)
{
	PkCompsData *comps_data = (PkCompsData*) (user_data);

	comps_data->query = COMPS_QUERY_CATEGORY;
	comps_data->current_query = g_strdup (category);

	comps_data->groups = g_ptr_array_new_with_free_func (g_free);

	if (!pk_backend_comps_parser (comps_data));
	   return comps_data->groups;

	g_ptr_array_add (comps_data->groups, NULL);
	return pk_ptr_array_to_strv (comps_data->groups);
}

/**
 * pk_backend_group_mapping:
 */
static gchar **
pk_backend_group_mapping (const gchar *mappgroup,
		gpointer user_data)
{
	guint i;
	guint len;
	gchar **category_groups = NULL;
	PkCompsData *comps_data = (PkCompsData*) (user_data);
	GPtrArray *groups =  g_ptr_array_new_with_free_func (g_free);

	if (g_strcmp0 (mappgroup, "internet") == 0) {
		g_ptr_array_add (groups, g_strdup ("graphical-internet"));
		g_ptr_array_add (groups, g_strdup ("text-internet"));
	}
	else if (g_strcmp0 (mappgroup, "legacy") == 0)
		g_ptr_array_add (groups, g_strdup ("legacy-software-support"));
	else if (g_strcmp0 (mappgroup, "publishing") == 0)
		g_ptr_array_add (groups, g_strdup ("authoring-and-publishing"));
	else if (g_strcmp0 (mappgroup, "desktop-kde") == 0)
		category_groups = pk_backend_get_groups_from_category ("kde-desktop-environment", comps_data);
	else if (g_strcmp0 (mappgroup, "desktop-gnome") == 0)
		category_groups = pk_backend_get_groups_from_category ("gnome-desktop-environment", comps_data);
	else if (g_strcmp0 (mappgroup, "desktop-xfce") == 0)
		category_groups = pk_backend_get_groups_from_category ("xfce-desktop-environment", comps_data);
	else if (g_strcmp0 (mappgroup, "desktop-other") == 0)
		category_groups = pk_backend_get_groups_from_category ("lxde-desktop-environment", comps_data);
	else if (g_strcmp0 (mappgroup, "programming") == 0)
		category_groups = pk_backend_get_groups_from_category ("development", comps_data);
	else if (g_strcmp0 (mappgroup, "servers") == 0)
		category_groups = pk_backend_get_groups_from_category ("servers", comps_data);
	else if (g_strcmp0 (mappgroup, "system") == 0)
		category_groups = pk_backend_get_groups_from_category ("base-system", comps_data);
	else g_ptr_array_add (groups, g_strdup (mappgroup));

	if (category_groups != NULL) {
		len = g_strv_length (category_groups);
		for (i = 0; i < len; i++)
			g_ptr_array_add (groups, g_strdup (category_groups[i]) );
	}
	return pk_ptr_array_to_strv (groups);
}

/**
 * pk_backend_search_groups:
 */
void
pk_backend_search_groups (PkBackend *backend,
		PkBackendJob *job,
		PkBitfield filters,
		gchar **values)
{
	guint i = 0;
	const gchar *comps_name;
	gchar **packages = NULL;
	HyQuery query = NULL;
	HySack sack = NULL;
	HifState *state_local = NULL;
	HyPackage pkg = NULL;
	HyPackageList plist = NULL;
	HifSource *src;
	PkBackendHifJobData *job_data = pk_backend_job_get_user_data (job);
	PkBackendHifPrivate *priv = pk_backend_get_user_data (backend);
	PkCompsData *comps_data = g_new0 (PkCompsData, 1);
	_cleanup_ptrarray_unref_ GPtrArray *sources = NULL;
	_cleanup_error_free_ GError *error = NULL;

	if (g_strv_length (values) ==  0)
		return;

	pk_backend_job_set_allow_cancel (job, TRUE);
	pk_backend_job_set_status (job, PK_STATUS_ENUM_QUERY);

	comps_data->comps = g_ptr_array_new_with_free_func (g_free);

	/* get sack */
	state_local = hif_state_get_child (job_data->state);
	sack = hif_utils_create_sack_for_filters (job,
						  filters,
						  HIF_CREATE_SACK_FLAG_USE_CACHE,
						  state_local,
						  &error);
	query = hy_query_create (sack);

	sources = hif_repos_get_sources (priv->repos, &error);

	if (sources == NULL) {
		pk_backend_job_error_code (job,
					   error->code,
					   "failed to scan yum.repos.d: %s",
					   error->message);
		return;
	}

	/* none? */
	if (sources->len == 0) {
		pk_backend_job_error_code (job,
					   PK_ERROR_ENUM_REPO_NOT_FOUND,
					   "failed to find any repos");
		return;
	}

	/* Create the comps_array to be parsed */
	for (i = 0; i < sources->len; i++) {
		src = g_ptr_array_index (sources, i);
		comps_name = hif_source_get_filename_md (src, "group");
		if (comps_name != NULL)
			g_ptr_array_add (comps_data->comps, g_strdup (comps_name));
	}

	packages = pk_backend_get_packages_from_group (pk_backend_group_mapping (values[0], comps_data), comps_data);

	hy_query_filter_in (query, HY_PKG_NAME, HY_EQ, (const char**)packages);
	plist = hy_query_run (query);
	hy_query_free (query);

	for (i = 0; i < (guint) hy_packagelist_count (plist); i++) {
		pkg = hy_packagelist_get_clone(plist, i);
		pk_backend_job_package (job,
				PK_INFO_ENUM_AVAILABLE,
				pk_package_id_build (
					hy_package_get_name (pkg),
					hy_package_get_version (pkg),
					hy_package_get_arch (pkg),
					hy_package_get_packager (pkg)),
				hy_package_get_summary (pkg));
	}
	hy_packagelist_free (plist);
	pk_backend_job_finished (job);
}
