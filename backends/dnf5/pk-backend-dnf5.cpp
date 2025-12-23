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
#include <libdnf5/repo/repo_query.hpp>
#include <libdnf5/rpm/arch.hpp>
#include <libdnf5/repo/package_downloader.hpp>
#include <libdnf5/base/goal.hpp>
#include <libdnf5/advisory/advisory_query.hpp>
#include <algorithm>
#include <vector>
#include <set>
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
    if (pkg.get_install_time() > 0) {
        repo_id = "installed";
    }
    
    std::string package_id = pkg.get_name() + ";" + evr + ";" + pkg.get_arch() + ";" + repo_id;
    
    pk_backend_job_package (job, info, package_id.c_str(), pkg.get_summary().c_str());
}

extern "C" {

const char *
pk_backend_get_description (PkBackend *backend)
{
    return "DNF5 package manager backend";
}

const char *
pk_backend_get_author (PkBackend *backend)
{
    return "Neal Gompa <neal@gompa.dev>";
}

gboolean
pk_backend_supports_parallelization (PkBackend *backend)
{
    return TRUE;
}

gchar **
pk_backend_get_mime_types (PkBackend *backend)
{
    const gchar *mime_types[] = { "application/x-rpm", NULL };
    return g_strdupv ((gchar **) mime_types);
}

PkBitfield
pk_backend_get_roles (PkBackend *backend)
{
    PkBitfield roles;
    roles = pk_bitfield_from_enums (
        PK_ROLE_ENUM_DOWNLOAD_PACKAGES,
        PK_ROLE_ENUM_GET_DETAILS,
        PK_ROLE_ENUM_GET_DETAILS_LOCAL,
        PK_ROLE_ENUM_GET_FILES,
        PK_ROLE_ENUM_GET_FILES_LOCAL,
        PK_ROLE_ENUM_GET_PACKAGES,
        PK_ROLE_ENUM_GET_REPO_LIST,
        PK_ROLE_ENUM_RESOLVE,
        PK_ROLE_ENUM_REFRESH_CACHE,
        PK_ROLE_ENUM_GET_UPDATES,
        PK_ROLE_ENUM_GET_UPDATE_DETAIL,
        PK_ROLE_ENUM_WHAT_PROVIDES,
        -1);
    return roles;
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
        
        // Load repositories
        auto repo_sack = dnf5_base->get_repo_sack();
        repo_sack->create_repos_from_system_configuration();
        // Ensure system repo is created before loading
        repo_sack->get_system_repo();
        repo_sack->load_repos();
        
        g_debug ("PkBackendDnf5: libdnf5 initialized. Repos loaded: %zu", repo_sack->size());
        
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

static void
dnf5_sort_and_emit (PkBackendJob *job, std::vector<libdnf5::rpm::Package> &pkgs)
{

    // Sort: Installed first, then Name, then Arch
    std::sort(pkgs.begin(), pkgs.end(), [](const libdnf5::rpm::Package &a, const libdnf5::rpm::Package &b) {
        bool a_installed = (a.get_install_time() > 0);
        bool b_installed = (b.get_install_time() > 0);
        if (a_installed != b_installed) return a_installed; // True (installed) comes first

        if (a.get_name() != b.get_name()) return a.get_name() < b.get_name();
        
        if (a.get_arch() != b.get_arch()) return a.get_arch() < b.get_arch();
        
        return a.get_evr() < b.get_evr();
    });

    std::set<std::string> seen_nevras;
    for (auto &pkg : pkgs) {
        std::string nevra = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch();
        // If duplicates (same NEVRA), show only the first one (which is installed if applicable, due to sort)
        if (seen_nevras.find(nevra) == seen_nevras.end()) {
            dnf5_emit_pkg(job, pkg);
            seen_nevras.insert(nevra);
        }
    }
}

static void
dnf5_apply_filters (libdnf5::rpm::PackageQuery &query, PkBitfield filters)
{
    g_debug("dnf5_apply_filters: filters=%" G_GUINT64_FORMAT, filters);
    // installed / available filter
    gboolean installed = pk_bitfield_contain (filters, PK_FILTER_ENUM_INSTALLED);
    gboolean available = pk_bitfield_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED);

    if (installed && !available) {
        query.filter_installed();
    } else if (!installed && available) {
        query.filter_available();
    }
    // If both are true, or both are false, we do nothing and search all.

    // arch
    if (pk_bitfield_contain (filters, PK_FILTER_ENUM_ARCH)) {
        auto vars = dnf5_base->get_vars();
        if (vars.is_valid()) {
            std::string arch = vars->get_value("arch");
            if (!arch.empty()) {
                query.filter_arch({arch, "noarch"});
            } else {
                query.filter_arch(libdnf5::rpm::get_supported_arches());
            }
        }
    } else if (pk_bitfield_contain (filters, PK_FILTER_ENUM_NOT_ARCH)) {
         // Not explicitly supported easily, but rare.
    }
    
    // Newest
    // Always filter latest per arch unless specific version requested?
    // PackageKit usually implies "latest" unless looking for specific details?
    // But PK_FILTER_ENUM_NEWEST exists. 
    // If I don't set it, maybe I get duplicates?
    if (pk_bitfield_contain (filters, PK_FILTER_ENUM_NEWEST)) {
        query.filter_latest_evr();
    }
}


void
pk_backend_search_names (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
    g_debug ("PkBackendDnf5: search_names");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        libdnf5::rpm::PackageQuery query(*dnf5_base);
        dnf5_apply_filters(query, filters);
        
        std::vector<std::string> search_terms;
        for (guint i = 0; values[i] != NULL; i++) {
             search_terms.emplace_back(values[i]);
        }
        
        query.filter_name(search_terms, libdnf5::sack::QueryCmp::ICONTAINS);
        
        std::vector<libdnf5::rpm::Package> pkgs;
        for (auto pkg : query) pkgs.push_back(pkg);
        dnf5_sort_and_emit(job, pkgs);
        
    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: Search failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_search_details (PkBackend *backend,
             PkBackendJob *job,
             PkBitfield filters,
             gchar **values)
{
    g_debug ("PkBackendDnf5: search_details");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        std::vector<std::string> search_terms;
        for (guint i = 0; values[i] != NULL; i++) {
             search_terms.emplace_back(values[i]);
        }

        std::vector<libdnf5::rpm::Package> pkgs;

        // Search by description
        libdnf5::rpm::PackageQuery query_desc(*dnf5_base);
        query_desc.filter_description(search_terms, libdnf5::sack::QueryCmp::ICONTAINS);
        dnf5_apply_filters(query_desc, filters);
        for (auto pkg : query_desc) pkgs.push_back(pkg);
    
        // Search by summary
        libdnf5::rpm::PackageQuery query_summary(*dnf5_base);
        query_summary.filter_summary(search_terms, libdnf5::sack::QueryCmp::ICONTAINS);
        dnf5_apply_filters(query_summary, filters);
        for (auto pkg : query_summary) pkgs.push_back(pkg);
        
        dnf5_sort_and_emit(job, pkgs);
        
    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: Search details failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_search_files (PkBackend *backend,
             PkBackendJob *job,
             PkBitfield filters,
             gchar **values)
{
    g_debug ("PkBackendDnf5: search_files");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        libdnf5::rpm::PackageQuery query(*dnf5_base);
        dnf5_apply_filters(query, filters);

        std::vector<std::string> search_terms;
        for (guint i = 0; values[i] != NULL; i++) {
             search_terms.emplace_back(values[i]);
        }
        
        query.filter_file(search_terms); 
        
        std::vector<libdnf5::rpm::Package> pkgs;
        for (auto pkg : query) pkgs.push_back(pkg);
        dnf5_sort_and_emit(job, pkgs);
        
    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: Search files failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_refresh_cache (PkBackend *backend, PkBackendJob *job, gboolean force)
{
    g_debug ("PkBackendDnf5: refresh_cache force=%d", force);
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        
        // Re-initialize base to allow reloading repos
        dnf5_base = std::make_unique<libdnf5::Base>();
        dnf5_base->load_config();
        dnf5_base->setup();

        auto repo_sack = dnf5_base->get_repo_sack();
        repo_sack->create_repos_from_system_configuration();
        
        if (force) {
            libdnf5::repo::RepoQuery q(*dnf5_base);
            q.filter_enabled(true);
            for (auto repo : q) {
                repo->expire();
            }
        }
        
        // Ensure system repo is created before loading
        repo_sack->get_system_repo();
        repo_sack->load_repos();
        
    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: Refresh cache failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_get_repo_list (PkBackend *backend,
              PkBackendJob *job,
              PkBitfield filters)
{
    g_debug ("PkBackendDnf5: get_repo_list");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        libdnf5::repo::RepoQuery query(*dnf5_base);
        for (auto repo : query) {
            bool enabled = repo->is_enabled();
            
            if (pk_bitfield_contain(filters, PK_FILTER_ENUM_INSTALLED) && !enabled) continue;
            if (pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_INSTALLED) && enabled) continue;

            // Filter out internal repos
            std::string id = repo->get_id();
            if (id == "@System" || id == "@commandline") continue;

            pk_backend_job_repo_detail(job,
                                       id.c_str(),
                                       repo->get_name().c_str(),
                                       enabled);
        }
    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: GetRepoList failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_get_packages (PkBackend *backend,
             PkBackendJob *job,
             PkBitfield filters)
{
    g_debug ("PkBackendDnf5: get_packages");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        libdnf5::rpm::PackageQuery query(*dnf5_base);
        dnf5_apply_filters(query, filters);
        
        std::vector<libdnf5::rpm::Package> pkgs;
        for (auto pkg : query) pkgs.push_back(pkg);
        dnf5_sort_and_emit(job, pkgs);
        
    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: GetPackages failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

static std::vector<libdnf5::rpm::Package>
dnf5_resolve_package_ids(gchar **package_ids)
{
    std::vector<libdnf5::rpm::Package> pkgs;
    if (!package_ids) return pkgs;
    
    for (int i = 0; package_ids[i] != NULL; i++) {
        gchar **split = pk_package_id_split(package_ids[i]);
        if (!split) continue;
        
        const char *name = split[PK_PACKAGE_ID_NAME];
        const char *version = split[PK_PACKAGE_ID_VERSION];
        const char *arch = split[PK_PACKAGE_ID_ARCH];
        const char *repo_id = split[PK_PACKAGE_ID_DATA];
        
        try {
            libdnf5::rpm::PackageQuery query(*dnf5_base);
            query.filter_name(name);
            query.filter_evr(version);
            query.filter_arch(arch);
            
            if (g_strcmp0(repo_id, "installed") == 0) {
                query.filter_installed();
            } else {
                 query.filter_repo_id(repo_id);
            }
            
            for (auto pkg : query) {
                pkgs.push_back(pkg);
                break;
            }
        } catch (...) {}
        
        g_strfreev(split);
    }
    return pkgs;
}

void
pk_backend_get_details (PkBackend *backend, PkBackendJob *job, gchar **package_ids)
{
    g_debug ("PkBackendDnf5: get_details");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }
        
        auto pkgs = dnf5_resolve_package_ids(package_ids);
        for (auto &pkg : pkgs) {
             std::string repo_id = pkg.get_repo_id();
             if (pkg.get_install_time() > 0) repo_id = "installed";
             
             std::string pid = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch() + ";" + repo_id;
             
             std::string license = pkg.get_license();
             if (license.empty()) license = "unknown";
             
             pk_backend_job_details(job,
                 pid.c_str(),
                 pkg.get_summary().c_str(),
                 license.c_str(),
                 PK_GROUP_ENUM_UNKNOWN,
                 pkg.get_description().c_str(),
                 pkg.get_url().c_str(),
                 pkg.get_install_size(),
                 pkg.get_download_size());
        }
        
    } catch (const std::exception &e) {
         pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_get_files (PkBackend *backend, PkBackendJob *job, gchar **package_ids)
{
    g_debug ("PkBackendDnf5: get_files");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        auto pkgs = dnf5_resolve_package_ids(package_ids);
        for (auto &pkg : pkgs) {
             std::string repo_id = pkg.get_repo_id();
             if (pkg.get_install_time() > 0) repo_id = "installed";
             std::string pid = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch() + ";" + repo_id;
             
             auto files_vec = pkg.get_files();
             std::vector<char*> files_c_str;
             for (const auto &f : files_vec) {
                 files_c_str.push_back(const_cast<char*>(f.c_str()));
             }
             files_c_str.push_back(nullptr);
             
             pk_backend_job_files(job, pid.c_str(), files_c_str.data());
        }

    } catch (const std::exception &e) {
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);

}

void
pk_backend_resolve (PkBackend *backend,
             PkBackendJob *job,
             PkBitfield filters,
             gchar **package_ids)
{
    g_debug ("PkBackendDnf5: resolve");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        libdnf5::rpm::PackageQuery query(*dnf5_base);
        dnf5_apply_filters(query, filters);
        
        std::vector<std::string> names;
        for (int i = 0; package_ids[i] != NULL; i++) {
            names.push_back(package_ids[i]);
        }
        
        // Exact match
        query.filter_name(names, libdnf5::sack::QueryCmp::EQ);
        
        std::vector<libdnf5::rpm::Package> pkgs;
        for (auto pkg : query) pkgs.push_back(pkg);
        dnf5_sort_and_emit(job, pkgs);
        
    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: Resolve failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_get_details_local (PkBackend *backend, PkBackendJob *job, gchar **files)
{
    g_debug ("PkBackendDnf5: get_details_local");
    try {
        // Use a temporary base to avoid polluting the global sack with local files
        libdnf5::Base local_base;
        local_base.load_config();
        
        // Disable GPG checks for local packages to avoid "unsupported" errors if keys missing
        auto &config = local_base.get_config();
        config.get_pkg_gpgcheck_option().set(false);
        config.get_localpkg_gpgcheck_option().set(false);
        
        local_base.setup();
        
        std::vector<std::string> file_paths;
        for (int i = 0; files[i] != NULL; i++) {
             file_paths.push_back(files[i]);
        }
        
        auto added_pkgs = local_base.get_repo_sack()->add_cmdline_packages(file_paths);
        
        for (const auto &pair : added_pkgs) {
            const auto &pkg = pair.second;
             // For local packages, repo_id is empty or @commandline?
             std::string repo_id = pkg.get_repo_id();
             if (repo_id.empty()) repo_id = "unknown"; 

             std::string pid = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch() + ";" + repo_id;
             
             std::string license = pkg.get_license();
             if (license.empty()) license = "unknown";
             
             pk_backend_job_details(job,
                 pid.c_str(),
                 pkg.get_summary().c_str(),
                 license.c_str(),
                 PK_GROUP_ENUM_UNKNOWN,
                 pkg.get_description().c_str(),
                 pkg.get_url().c_str(),
                 pkg.get_install_size(),
                 0); // Download size 0 for local files
        }

    } catch (const std::exception &e) {
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_get_files_local (PkBackend *backend, PkBackendJob *job, gchar **files)
{
    g_debug ("PkBackendDnf5: get_files_local");
    try {
        // Use a temporary base
        libdnf5::Base local_base;
        local_base.load_config();
        
        // Disable GPG checks for local packages
        auto &config = local_base.get_config();
        config.get_pkg_gpgcheck_option().set(false);
        config.get_localpkg_gpgcheck_option().set(false);

        local_base.setup();
        
        std::vector<std::string> file_paths;
        for (int i = 0; files[i] != NULL; i++) {
             file_paths.push_back(files[i]);
        }
        
        auto added_pkgs = local_base.get_repo_sack()->add_cmdline_packages(file_paths);

        for (const auto &pair : added_pkgs) {
            const auto &pkg = pair.second;
             std::string repo_id = pkg.get_repo_id();
             if (repo_id.empty()) repo_id = "unknown";

             std::string pid = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch() + ";" + repo_id;
             
             auto files_vec = pkg.get_files();
             std::vector<char*> files_c_str;
             for (const auto &f : files_vec) {
                 files_c_str.push_back(const_cast<char*>(f.c_str()));
             }
             files_c_str.push_back(nullptr);
             
             pk_backend_job_files(job, pid.c_str(), files_c_str.data());
        }

    } catch (const std::exception &e) {
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_download_packages (PkBackend *backend,
                              PkBackendJob *job,
                              gchar **package_ids,
                              const gchar *directory)
{
    g_debug ("PkBackendDnf5: download_packages to %s", directory);
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        auto pkgs = dnf5_resolve_package_ids(package_ids);
        // Use PackageDownloader
        libdnf5::repo::PackageDownloader downloader(*dnf5_base);
        std::vector<std::string> downloaded_paths;
        
        for (auto &pkg : pkgs) {
             std::string repo_id = pkg.get_repo_id();
             if (pkg.get_install_time() > 0) repo_id = "installed";
             std::string pid = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch() + ";" + repo_id;

             pk_backend_job_package(job, PK_INFO_ENUM_DOWNLOADING, pid.c_str(), pkg.get_summary().c_str());

             if (repo_id == "installed") continue; // Skip installed
             
             // Add to downloader
             downloader.add(pkg, directory);
             
             // Predict path for reporting
             std::string filename = pkg.get_name() + "-" + pkg.get_evr() + "." + pkg.get_arch() + ".rpm";
             std::string target_path = std::string(directory) + "/" + filename;
             downloaded_paths.push_back(target_path);
        }
        
        // Perform download
        downloader.download();
        
        std::vector<char*> files_c_str;
        for (const auto &p : downloaded_paths) {
            files_c_str.push_back(const_cast<char*>(p.c_str()));
        }
        files_c_str.push_back(nullptr);
        
        pk_backend_job_files(job, NULL, files_c_str.data());

    } catch (const std::exception &e) {
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_get_updates (PkBackend *backend,
                        PkBackendJob *job,
                        PkBitfield filters)
{
    g_debug ("PkBackendDnf5: get_updates");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        libdnf5::Goal goal(*dnf5_base);
        goal.add_rpm_upgrade();
        
        libdnf5::base::Transaction transaction = goal.resolve();
        auto transaction_items = transaction.get_transaction_packages();
        
        for (const auto &item : transaction_items) {
             auto action = item.get_action();
             if (action != libdnf5::transaction::TransactionItemAction::UPGRADE &&
                 action != libdnf5::transaction::TransactionItemAction::INSTALL) {
                  continue;
             }
             
             auto pkg = item.get_package();
             std::string repo_id = pkg.get_repo_id();
             std::string pid = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch() + ";" + repo_id;
             
             // Emit as AVAILABLE (standard for updates list in PK)
             pk_backend_job_package(job, PK_INFO_ENUM_AVAILABLE, pid.c_str(), pkg.get_summary().c_str());
        }

    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: GetUpdates failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_get_update_detail (PkBackend *backend,
                              PkBackendJob *job,
                              gchar **package_ids)
{
    g_debug ("PkBackendDnf5: get_update_detail");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        auto pkgs = dnf5_resolve_package_ids(package_ids);
        if (pkgs.empty()) {
             pk_backend_job_finished(job);
             return;
        }

        GPtrArray *update_details_array = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

        for (const auto &pkg : pkgs) {
             libdnf5::advisory::AdvisoryQuery query(*dnf5_base);
             libdnf5::rpm::Nevra nevra;
             nevra.set_name(pkg.get_name());
             nevra.set_epoch(pkg.get_epoch());
             nevra.set_version(pkg.get_version());
             nevra.set_release(pkg.get_release());
             nevra.set_arch(pkg.get_arch());
             std::vector<libdnf5::rpm::Nevra> single_nevra = { nevra };
             query.filter_packages(single_nevra);
             
             std::string update_text;
             GPtrArray *vendor_urls = g_ptr_array_new_with_free_func (g_free);
             GPtrArray *bugzilla_urls = g_ptr_array_new_with_free_func (g_free);
             GPtrArray *cve_urls = g_ptr_array_new_with_free_func (g_free);
             
             for (const auto &advisory : query) {
                  if (!update_text.empty()) update_text += "\n\n";
                  update_text += advisory.get_description();
                  
                  for (const auto &ref : advisory.get_references()) {
                       std::string url = ref.get_url();
                       if (url.empty()) continue;
                       
                       // Simple heuristic
                       if (url.find("bugzilla") != std::string::npos) g_ptr_array_add(bugzilla_urls, g_strdup(url.c_str()));
                       else if (url.find("cve") != std::string::npos) g_ptr_array_add(cve_urls, g_strdup(url.c_str()));
                       else g_ptr_array_add(vendor_urls, g_strdup(url.c_str()));
                  }
             }

             g_ptr_array_add(vendor_urls, NULL);
             g_ptr_array_add(bugzilla_urls, NULL);
             g_ptr_array_add(cve_urls, NULL);
             
             std::string repo_id = pkg.get_repo_id();
             std::string pid = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch() + ";" + repo_id;
             
             PkUpdateDetail *item = pk_update_detail_new();
             g_object_set(item,
                 "package-id", pid.c_str(),
                 "updates", NULL,
                 "obsoletes", NULL,
                 "vendor-urls", (gchar**)vendor_urls->pdata,
                 "bugzilla-urls", (gchar**)bugzilla_urls->pdata,
                 "cve-urls", (gchar**)cve_urls->pdata,
                 "restart", PK_RESTART_ENUM_NONE, 
                 "update-text", update_text.c_str(),
                 "changelog", NULL,
                 "state", PK_UPDATE_STATE_ENUM_STABLE, 
                 "issued", NULL,
                 "updated", NULL,
                 NULL);
             
             g_ptr_array_add(update_details_array, item);
             g_ptr_array_unref(vendor_urls);
             g_ptr_array_unref(bugzilla_urls);
             g_ptr_array_unref(cve_urls);
        }
        
        pk_backend_job_update_details(job, update_details_array);
        g_ptr_array_unref(update_details_array);

    } catch (const std::exception &e) {
        g_warning ("PkBackendDnf5: GetUpdateDetail failed: %s", e.what());
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

void
pk_backend_what_provides (PkBackend *backend,
                          PkBackendJob *job,
                          PkBitfield filters,
                          gchar **search)
{
    g_debug ("PkBackendDnf5: what_provides");
    try {
        std::lock_guard<std::mutex> lock(dnf5_mutex);
        if (!dnf5_base) {
             pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "Backend not initialized");
             pk_backend_job_finished (job);
             return;
        }

        // Decompose search terms
        std::vector<std::string> provides;
        for (gchar **s = search; *s != nullptr; s++) {
             std::string term = *s;
             // Logic from pk-backend-dnf.c:pk_backend_what_provides_decompose
             provides.push_back(term);
             provides.push_back("gstreamer0.10(" + term + ")");
             provides.push_back("gstreamer1(" + term + ")");
             provides.push_back("font(" + term + ")");
             provides.push_back("mimehandler(" + term + ")");
             provides.push_back("postscriptdriver(" + term + ")");
             provides.push_back("plasma4(" + term + ")");
             provides.push_back("plasma5(" + term + ")");
             provides.push_back("language(" + term + ")");
        }

        libdnf5::rpm::PackageQuery query(*dnf5_base);
        query.filter_provides(provides);
        dnf5_apply_filters(query, filters);
        
        std::vector<libdnf5::rpm::Package> pkg_vector(query.begin(), query.end());
        g_debug ("WhatProvides: Found %zu packages", pkg_vector.size());
        dnf5_sort_and_emit(job, pkg_vector);

    } catch (const std::exception &e) {
        pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
    }
    pk_backend_job_finished (job);
}

}
