/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2025 Neal Gompa <neal@gompa.dev>
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

#include <pk-backend.h>
#include <libdnf5/base/base.hpp>
#include <libdnf5/conf/config_parser.hpp>
#include <libdnf5/logger/logger.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <glib.h>

// Global definitions
static std::unique_ptr<libdnf5::Base> dnf5_base;
static std::mutex dnf5_mutex;


static void
dnf5_emit_pkg (PkBackendJob *job, const libdnf5::rpm::Package &pkg)
{
    PkInfoEnum info = PK_INFO_ENUM_AVAILABLE;
    if (pkg.get_install_time() > 0) {
        info = PK_INFO_ENUM_INSTALLED;
    }
    
    // Construct package ID: name;version;arch;repo_id
    // EVR: epoch:version-release
    std::string evr = pkg.get_evr();
    // Repo ID
    std::string repo_id = pkg.get_repo_id();
    
    std::string package_id = pkg.get_name() + ";" + evr + ";" + pkg.get_arch() + ";" + repo_id;
    
    pk_backend_job_package (job, info, package_id.c_str(), pkg.get_summary().c_str());
}

extern "C" {

const char *
pk_backend_get_description (PkBackend *backend)
{
    return "DNF5 Backend";
}

const char *
pk_backend_get_author (PkBackend *backend)
{
    return "Neal Gompa";
}

gboolean
pk_backend_supports_parallelization (PkBackend *backend)
{
    return TRUE;
}

void
pk_backend_initialize (GKeyFile *conf, PkBackend *backend)
{
    g_debug ("PkBackendDnf5: initialize");
    
    // Initialize libdnf5 base
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        dnf5_base = std::make_unique<libdnf5::Base>();
        
        // Load configuration
        dnf5_base->load_config();
        dnf5_base->setup();
        
        g_debug ("PkBackendDnf5: libdnf5 initialized successfully");
    } catch (const std::exception &e) {
        g_error ("PkBackendDnf5: Failed to initialize libdnf5: %s", e.what());
    }
}

void
pk_backend_destroy (PkBackend *backend)
{
    g_debug ("PkBackendDnf5: destroy");
    std::lock_guard<std::mutex> lock(dnf5_mutex);
    dnf5_base.reset();
}

void
pk_backend_start_job (PkBackend *backend, PkBackendJob *job)
{
    std::lock_guard<std::mutex> lock(dnf5_mutex);
    if (!dnf5_base) {
         g_warning ("PkBackendDnf5: Base not initialized!");
         pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
         pk_backend_job_finished (job);
         return;
    }
    // No specific start logic needed if we implement individual methods
    pk_backend_job_set_status (job, PK_STATUS_ENUM_RUNNING);
}

void
pk_backend_stop_job (PkBackend *backend, PkBackendJob *job)
{
    g_debug ("PkBackendDnf5: stop_job");
}

void
pk_backend_search_names (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
    g_debug ("PkBackendDnf5: search_names");
    try {
        // libdnf::rpm::PackageQuery query(*dnf5_base);
        // We use lock to ensure base is valid, though it should be.
        
        for (guint i = 0; values[i] != NULL; i++) {
            g_debug ("Searching for: %s", values[i]);
             libdnf5::rpm::PackageQuery q(*dnf5_base);
             q.filter_name(values[i]); 
             
             for (const auto &pkg : q) {
                 dnf5_emit_pkg(job, pkg);
             }
        }
    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: Search failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, e.what());
    }
    pk_backend_job_finished (job);
}

}
