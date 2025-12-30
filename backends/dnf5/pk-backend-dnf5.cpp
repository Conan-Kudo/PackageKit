/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
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
#include <libdnf5/rpm/reldep_list.hpp>
#include <libdnf5/base/transaction.hpp>
#include <algorithm>
#include <vector>
#include <set>
#include <queue>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <filesystem>
#include <map>

#include <glib.h>

// Private data structures
typedef struct {
	std::unique_ptr<libdnf5::Base> base;
	GMutex mutex;
} PkBackendDnf5Private;

static void
dnf5_setup_base (PkBackendDnf5Private *priv)
{
	priv->base = std::make_unique<libdnf5::Base>();
	priv->base->load_config();
	priv->base->setup();
	auto repo_sack = priv->base->get_repo_sack();
	repo_sack->create_repos_from_system_configuration();
	repo_sack->get_system_repo();
	repo_sack->load_repos();
}

// Helper functions (Internal)

static std::vector<libdnf5::rpm::Package>
dnf5_process_dependency (libdnf5::Base &base, const libdnf5::rpm::Package &pkg, PkRoleEnum role, gboolean recursive)
{
	std::vector<libdnf5::rpm::Package> results;
	std::set<std::string> visited;
	std::queue<libdnf5::rpm::Package> queue;
	queue.push(pkg);
	visited.insert(pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch());
	
	while (!queue.empty()) {
		auto curr = queue.front();
		queue.pop();
		libdnf5::rpm::ReldepList reldeps(base);
		if (role == PK_ROLE_ENUM_DEPENDS_ON) reldeps = curr.get_requires();
		else reldeps = curr.get_provides();
		
		for (const auto &reldep : reldeps) {
			std::string req = reldep.to_string();
			libdnf5::rpm::PackageQuery query(base);
			if (role == PK_ROLE_ENUM_DEPENDS_ON) query.filter_provides(req);
			else query.filter_requires(req);
			
			for (const auto &res : query) {
				std::string res_nevra = res.get_name() + ";" + res.get_evr() + ";" + res.get_arch();
				if (visited.find(res_nevra) == visited.end()) {
					visited.insert(res_nevra);
					results.push_back(res);
					if (recursive) queue.push(res);
				}
			}
		}
	}
	return results;
}

static void
dnf5_emit_pkg (PkBackendJob *job, const libdnf5::rpm::Package &pkg, PkInfoEnum info = PK_INFO_ENUM_UNKNOWN)
{
	if (info == PK_INFO_ENUM_UNKNOWN) {
		info = PK_INFO_ENUM_AVAILABLE;
		if (pkg.get_install_time() > 0) {
			info = PK_INFO_ENUM_INSTALLED;
		}
	}
	
	std::string evr = pkg.get_evr();
	std::string repo_id = pkg.get_repo_id();
	if (pkg.get_install_time() > 0) {
		repo_id = "installed";
	}
	
	std::string package_id = pkg.get_name() + ";" + evr + ";" + pkg.get_arch() + ";" + repo_id;
	pk_backend_job_package (job, info, package_id.c_str(), pkg.get_summary().c_str());
}

static void
dnf5_sort_and_emit (PkBackendJob *job, std::vector<libdnf5::rpm::Package> &pkgs)
{
	std::sort(pkgs.begin(), pkgs.end(), [](const libdnf5::rpm::Package &a, const libdnf5::rpm::Package &b) {
		bool a_installed = (a.get_install_time() > 0);
		bool b_installed = (b.get_install_time() > 0);
		if (a_installed != b_installed) return a_installed; 
		if (a.get_name() != b.get_name()) return a.get_name() < b.get_name();
		if (a.get_arch() != b.get_arch()) return a.get_arch() < b.get_arch();
		return a.get_evr() < b.get_evr();
	});

	std::set<std::string> seen_nevras;
	for (auto &pkg : pkgs) {
		std::string nevra = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch();
		if (seen_nevras.find(nevra) == seen_nevras.end()) {
			dnf5_emit_pkg(job, pkg);
			seen_nevras.insert(nevra);
		}
	}
}

static void
dnf5_apply_filters (libdnf5::Base &base, libdnf5::rpm::PackageQuery &query, PkBitfield filters)
{
	gboolean installed = pk_bitfield_contain (filters, PK_FILTER_ENUM_INSTALLED);
	gboolean available = pk_bitfield_contain (filters, PK_FILTER_ENUM_NOT_INSTALLED);

	if (installed && !available) {
		query.filter_installed();
	} else if (!installed && available) {
		query.filter_available();
	}

	if (pk_bitfield_contain (filters, PK_FILTER_ENUM_ARCH)) {
		auto vars = base.get_vars();
		if (vars.is_valid()) {
			std::string arch = vars->get_value("arch");
			if (!arch.empty()) {
				query.filter_arch({arch, "noarch"});
			} else {
				query.filter_arch(libdnf5::rpm::get_supported_arches());
			}
		}
	}

	if (pk_bitfield_contain (filters, PK_FILTER_ENUM_NEWEST)) {
		query.filter_latest_evr();
	}
}

static std::vector<libdnf5::rpm::Package>
dnf5_resolve_package_ids(libdnf5::Base &base, gchar **package_ids)
{
	std::vector<libdnf5::rpm::Package> pkgs;
	if (!package_ids) return pkgs;
	
	for (int i = 0; package_ids[i] != NULL; i++) {
		g_auto(GStrv) split = pk_package_id_split(package_ids[i]);
		if (!split) continue;
		
		try {
			libdnf5::rpm::PackageQuery query(base);
			query.filter_name(split[PK_PACKAGE_ID_NAME]);
			query.filter_evr(split[PK_PACKAGE_ID_VERSION]);
			query.filter_arch(split[PK_PACKAGE_ID_ARCH]);
			
			if (g_strcmp0(split[PK_PACKAGE_ID_DATA], "installed") == 0) {
				query.filter_installed();
			} else {
				 query.filter_repo_id(split[PK_PACKAGE_ID_DATA]);
			}
			
			for (auto pkg : query) {
				pkgs.push_back(pkg);
				break;
			}
		} catch (...) {}
	}
	return pkgs;
}

// Thread Workers

static void
dnf5_query_thread (PkBackendJob *job, GVariant *params, gpointer user_data)
{
	PkBackend *backend = (PkBackend *) pk_backend_job_get_backend (job);
	PkBackendDnf5Private *priv = (PkBackendDnf5Private *) pk_backend_get_user_data (backend);
	PkRoleEnum role = pk_backend_job_get_role (job);
	
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->mutex);
	
	try {
		if (role == PK_ROLE_ENUM_SEARCH_NAME || role == PK_ROLE_ENUM_SEARCH_DETAILS || role == PK_ROLE_ENUM_SEARCH_FILE || role == PK_ROLE_ENUM_RESOLVE || role == PK_ROLE_ENUM_WHAT_PROVIDES) {
			PkBitfield filters;
			g_auto(GStrv) values = NULL;
			g_variant_get (params, "(t^as)", &filters, &values);
			
			std::vector<libdnf5::rpm::Package> results;
			libdnf5::rpm::PackageQuery query(*priv->base);
			dnf5_apply_filters(*priv->base, query, filters);
			
			std::vector<std::string> search_terms;
			for (int i = 0; values[i]; i++) search_terms.push_back(values[i]);
			
			if (role == PK_ROLE_ENUM_SEARCH_NAME) {
				query.filter_name(search_terms, libdnf5::sack::QueryCmp::ICONTAINS);
			} else if (role == PK_ROLE_ENUM_SEARCH_FILE) {
				query.filter_file(search_terms);
			} else if (role == PK_ROLE_ENUM_RESOLVE) {
				query.filter_name(search_terms, libdnf5::sack::QueryCmp::EQ);
			} else if (role == PK_ROLE_ENUM_WHAT_PROVIDES) {
				std::vector<std::string> provides;
				for (const auto &term : search_terms) {
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
				query.filter_provides(provides);
			} else if (role == PK_ROLE_ENUM_SEARCH_DETAILS) {
				libdnf5::rpm::PackageQuery query_sum(*priv->base);
				dnf5_apply_filters(*priv->base, query_sum, filters);
				query.filter_description(search_terms, libdnf5::sack::QueryCmp::ICONTAINS);
				query_sum.filter_summary(search_terms, libdnf5::sack::QueryCmp::ICONTAINS);
				for (auto p : query_sum) results.push_back(p);
			}
			
			for (auto p : query) results.push_back(p);
			dnf5_sort_and_emit(job, results);
			
		} else if (role == PK_ROLE_ENUM_DEPENDS_ON || role == PK_ROLE_ENUM_REQUIRED_BY) {
			PkBitfield filters;
			g_auto(GStrv) package_ids = NULL;
			gboolean recursive;
			g_variant_get (params, "(t^asb)", &filters, &package_ids, &recursive);
			
			auto input_pkgs = dnf5_resolve_package_ids(*priv->base, package_ids);
			std::vector<libdnf5::rpm::Package> results;
			for (const auto &pkg : input_pkgs) {
				auto deps = dnf5_process_dependency(*priv->base, pkg, role, recursive);
				results.insert(results.end(), deps.begin(), deps.end());
			}
			dnf5_sort_and_emit(job, results);

		} else if (role == PK_ROLE_ENUM_GET_PACKAGES || role == PK_ROLE_ENUM_GET_UPDATES) {
			PkBitfield filters;
			g_variant_get (params, "(t)", &filters);
			
			libdnf5::rpm::PackageQuery query(*priv->base);
			dnf5_apply_filters(*priv->base, query, filters);
			
			if (role == PK_ROLE_ENUM_GET_UPDATES) {
				libdnf5::Goal goal(*priv->base);
				goal.add_rpm_upgrade();
				auto trans = goal.resolve();
				for (const auto &item : trans.get_transaction_packages()) {
					auto action = item.get_action();
					if (action == libdnf5::transaction::TransactionItemAction::UPGRADE || action == libdnf5::transaction::TransactionItemAction::INSTALL) {
						dnf5_emit_pkg(job, item.get_package());
					}
				}
			} else {
				std::vector<libdnf5::rpm::Package> results;
				for (auto p : query) results.push_back(p);
				dnf5_sort_and_emit(job, results);
			}
		} else if (role == PK_ROLE_ENUM_GET_DETAILS || role == PK_ROLE_ENUM_GET_FILES || role == PK_ROLE_ENUM_DOWNLOAD_PACKAGES || role == PK_ROLE_ENUM_GET_UPDATE_DETAIL) {
			g_auto(GStrv) package_ids = NULL;
			if (role == PK_ROLE_ENUM_DOWNLOAD_PACKAGES) {
				gchar *directory = NULL;
				g_variant_get (params, "(^as&s)", &package_ids, &directory);
				auto pkgs = dnf5_resolve_package_ids(*priv->base, package_ids);
				libdnf5::repo::PackageDownloader downloader(*priv->base);
				for (auto &pkg : pkgs) {
					dnf5_emit_pkg(job, pkg, PK_INFO_ENUM_DOWNLOADING);
					downloader.add(pkg, directory);
				}
				downloader.download();
				
				std::vector<char*> files_c;
				for (auto &pkg : pkgs) {
					std::string path = pkg.get_package_path();
					if (!path.empty()) files_c.push_back(g_strdup(path.c_str()));
				}
				files_c.push_back(nullptr);
				pk_backend_job_files (job, NULL, files_c.data());
				for (auto p : files_c) g_free(p);
				pk_backend_job_finished (job);
				return;
			} else {
				g_variant_get (params, "(^as)", &package_ids);
			}
			
			auto pkgs = dnf5_resolve_package_ids(*priv->base, package_ids);
			for (auto &pkg : pkgs) {
				std::string repo_id = pkg.get_repo_id();
				if (pkg.get_install_time() > 0) repo_id = "installed";
				std::string pid = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch() + ";" + repo_id;
				
				if (role == PK_ROLE_ENUM_GET_DETAILS || role == PK_ROLE_ENUM_GET_UPDATE_DETAIL) {
					std::string license = pkg.get_license();
					if (license.empty()) license = "unknown";
					pk_backend_job_details(job, pid.c_str(), pkg.get_summary().c_str(), license.c_str(), PK_GROUP_ENUM_UNKNOWN, pkg.get_description().c_str(), pkg.get_url().c_str(), pkg.get_install_size(), pkg.get_download_size());
				} else if (role == PK_ROLE_ENUM_GET_FILES) {
					auto files_vec = pkg.get_files();
					std::vector<char*> files_c;
					for (const auto &f : files_vec) files_c.push_back(const_cast<char*>(f.c_str()));
					files_c.push_back(nullptr);
					pk_backend_job_files(job, pid.c_str(), files_c.data());
				}
			}
		} else if (role == PK_ROLE_ENUM_GET_DETAILS_LOCAL || role == PK_ROLE_ENUM_GET_FILES_LOCAL) {
			g_auto(GStrv) files = NULL;
			g_variant_get (params, "(^as)", &files);
			libdnf5::Base local_base;
			local_base.load_config();
			local_base.get_config().get_pkg_gpgcheck_option().set(false);
			local_base.setup();
			std::vector<std::string> paths;
			for (int i = 0; files[i]; i++) paths.push_back(files[i]);
			auto added = local_base.get_repo_sack()->add_cmdline_packages(paths);
			for (const auto &pair : added) {
				const auto &pkg = pair.second;
				std::string pid = pkg.get_name() + ";" + pkg.get_evr() + ";" + pkg.get_arch() + ";" + (pkg.get_repo_id().empty() ? "local" : pkg.get_repo_id());
				if (role == PK_ROLE_ENUM_GET_DETAILS_LOCAL) {
					pk_backend_job_details(job, pid.c_str(), pkg.get_summary().c_str(), pkg.get_license().c_str(), PK_GROUP_ENUM_UNKNOWN, pkg.get_description().c_str(), pkg.get_url().c_str(), pkg.get_install_size(), 0);
				} else {
					auto files_vec = pkg.get_files();
					std::vector<char*> files_c;
					for (const auto &f : files_vec) files_c.push_back(const_cast<char*>(f.c_str()));
					files_c.push_back(nullptr);
					pk_backend_job_files(job, pid.c_str(), files_c.data());
				}
			}
		} else if (role == PK_ROLE_ENUM_GET_REPO_LIST) {
			PkBitfield filters;
			g_variant_get (params, "(t)", &filters);
			libdnf5::repo::RepoQuery query(*priv->base);
			for (auto repo : query) {
				std::string id = repo->get_id();
				if (id == "@System" || id == "@commandline") continue;
				bool enabled = repo->is_enabled();
				if (pk_bitfield_contain(filters, PK_FILTER_ENUM_INSTALLED) && !enabled) continue;
				if (pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_INSTALLED) && enabled) continue;
				pk_backend_job_repo_detail(job, id.c_str(), repo->get_name().c_str(), enabled);
			}
		}
	} catch (const std::exception &e) {
		pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
	}
	pk_backend_job_finished (job);
}

static void
dnf5_transaction_thread (PkBackendJob *job, GVariant *params, gpointer user_data)
{
	PkBackend *backend = (PkBackend *) pk_backend_job_get_backend (job);
	PkBackendDnf5Private *priv = (PkBackendDnf5Private *) pk_backend_get_user_data (backend);
	PkRoleEnum role = pk_backend_job_get_role (job);
	
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->mutex);
	
	try {
		libdnf5::Goal goal(*priv->base);
		PkBitfield transaction_flags = 0;
		
		if (role == PK_ROLE_ENUM_INSTALL_PACKAGES || role == PK_ROLE_ENUM_UPDATE_PACKAGES || role == PK_ROLE_ENUM_REMOVE_PACKAGES) {
			g_auto(GStrv) package_ids = NULL;
			if (role == PK_ROLE_ENUM_REMOVE_PACKAGES) {
				gboolean allow_deps, autoremove;
				g_variant_get (params, "(t^asbb)", &transaction_flags, &package_ids, &allow_deps, &autoremove);
				if (autoremove) priv->base->get_config().get_clean_requirements_on_remove_option().set(true);
			} else {
				g_variant_get (params, "(t^as)", &transaction_flags, &package_ids);
			}
			
			auto pkgs = dnf5_resolve_package_ids(*priv->base, package_ids);
			if (pkgs.empty() && role != PK_ROLE_ENUM_UPDATE_PACKAGES) {
				pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_NOT_FOUND, "No packages found");
				pk_backend_job_finished (job);
				return;
			}
			
			for (auto &pkg : pkgs) {
				std::string spec = pkg.get_name() + "-" + pkg.get_evr() + "." + pkg.get_arch();
				if (role == PK_ROLE_ENUM_INSTALL_PACKAGES) goal.add_install(spec);
				else if (role == PK_ROLE_ENUM_REMOVE_PACKAGES) goal.add_remove(spec);
				else if (role == PK_ROLE_ENUM_UPDATE_PACKAGES) goal.add_rpm_upgrade(spec);
			}
			if (role == PK_ROLE_ENUM_UPDATE_PACKAGES && pkgs.empty()) goal.add_rpm_upgrade();
			
		} else if (role == PK_ROLE_ENUM_INSTALL_FILES) {
			g_auto(GStrv) full_paths = NULL;
			g_variant_get (params, "(t^as)", &transaction_flags, &full_paths);
			std::vector<std::string> paths;
			for (int i = 0; full_paths[i]; i++) paths.push_back(full_paths[i]);
			auto added = priv->base->get_repo_sack()->add_cmdline_packages(paths);
			for (const auto &p : added) goal.add_install(p.second.get_name() + "-" + p.second.get_evr() + "." + p.second.get_arch());
		} else if (role == PK_ROLE_ENUM_UPGRADE_SYSTEM) {
			gchar *distro_id = NULL;
			PkUpgradeKindEnum upgrade_kind;
			g_variant_get (params, "(t&su)", &transaction_flags, &distro_id, &upgrade_kind);
			if (distro_id) priv->base->get_vars()->set("releasever", distro_id);
			goal.add_rpm_distro_sync();
		} else if (role == PK_ROLE_ENUM_REPAIR_SYSTEM) {
			g_variant_get (params, "(t)", &transaction_flags);
			if (pk_bitfield_contain (transaction_flags, PK_TRANSACTION_FLAG_ENUM_SIMULATE)) {
				pk_backend_job_finished (job);
				return;
			}
			std::filesystem::path rpm_db_path("/var/lib/rpm");
			if (std::filesystem::exists(rpm_db_path) && std::filesystem::is_directory(rpm_db_path)) {
				for (const auto& entry : std::filesystem::directory_iterator(rpm_db_path)) {
					if (entry.is_regular_file() && entry.path().filename().string().starts_with("__db.")) {
						std::filesystem::remove(entry.path());
					}
				}
			}
			pk_backend_job_finished (job);
			return;
		}
		
		pk_backend_job_set_status (job, PK_STATUS_ENUM_QUERY);
		auto trans = goal.resolve();
		auto problems = trans.get_transaction_problems();
		if (!problems.empty()) {
			std::string msg;
			for (const auto &p : problems) msg += p + "; ";
			pk_backend_job_error_code (job, PK_ERROR_ENUM_DEP_RESOLUTION_FAILED, "%s", msg.c_str());
			pk_backend_job_finished (job);
			return;
		}
		
		if (pk_bitfield_contain (transaction_flags, PK_TRANSACTION_FLAG_ENUM_SIMULATE)) {
			for (const auto &item : trans.get_transaction_packages()) {
				dnf5_emit_pkg(job, item.get_package());
			}
			pk_backend_job_finished (job);
			return;
		}
		
		pk_backend_job_set_status (job, PK_STATUS_ENUM_DOWNLOAD);
		trans.download();
		pk_backend_job_set_status (job, PK_STATUS_ENUM_RUNNING);
		trans.run();
		
		// Post-transaction base re-initialization to ensure state consistency
		dnf5_setup_base (priv);
		
	} catch (const std::exception &e) {
		pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
	}
	pk_backend_job_finished (job);
}

static void
dnf5_repo_thread (PkBackendJob *job, GVariant *params, gpointer user_data)
{
	PkBackend *backend = (PkBackend *) pk_backend_job_get_backend (job);
	PkBackendDnf5Private *priv = (PkBackendDnf5Private *) pk_backend_get_user_data (backend);
	PkRoleEnum role = pk_backend_job_get_role (job);
	
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->mutex);
	
	try {
		if (role == PK_ROLE_ENUM_REPO_ENABLE || role == PK_ROLE_ENUM_REPO_SET_DATA) {
			gchar *repo_id = NULL;
			const gchar *parameter, *value;
			if (role == PK_ROLE_ENUM_REPO_ENABLE) {
				gboolean enabled;
				g_variant_get (params, "(&sb)", &repo_id, &enabled);
				parameter = "enabled";
				value = enabled ? "1" : "0";
			} else {
				g_variant_get (params, "(&s&s&s)", &repo_id, &parameter, &value);
			}
			
			libdnf5::repo::RepoQuery query(*priv->base);
			query.filter_id(repo_id);
			for (auto repo : query) {
				if (g_strcmp0(parameter, "enabled") == 0) {
					bool enable = (g_strcmp0(value, "1") == 0 || g_strcmp0(value, "true") == 0);
					if (repo->is_enabled() == enable) {
						pk_backend_job_error_code (job, PK_ERROR_ENUM_REPO_ALREADY_SET, "Repo already in state");
						pk_backend_job_finished (job);
						return;
					}
					if (enable) repo->enable(); else repo->disable();
					libdnf5::ConfigParser parser;
					parser.read(repo->get_repo_file_path());
					parser.set_value(repo_id, "enabled", value);
					parser.write(repo->get_repo_file_path(), false);
				}
			}
		} else if (role == PK_ROLE_ENUM_REPO_REMOVE) {
			gchar *repo_id = NULL;
			gboolean autoremove;
			PkBitfield transaction_flags;
			g_variant_get (params, "(t&sb)", &transaction_flags, &repo_id, &autoremove);
			
			libdnf5::repo::RepoQuery query(*priv->base);
			query.filter_id(repo_id);
			std::string repo_file;
			for (auto repo : query) {
				repo_file = repo->get_repo_file_path();
				break;
			}
			
			if (repo_file.empty()) {
				pk_backend_job_error_code (job, PK_ERROR_ENUM_REPO_NOT_FOUND, "Repo %s not found", repo_id);
				pk_backend_job_finished (job);
				return;
			}
			
			// Find all repos in the same file to track all packages that should be removed
			std::vector<std::string> all_repo_ids;
			libdnf5::repo::RepoQuery all_repos_query(*priv->base);
			for (auto repo : all_repos_query) {
				if (repo->get_repo_file_path() == repo_file) {
					all_repo_ids.push_back(repo->get_id());
				}
			}

			libdnf5::Goal goal(*priv->base);
			
			// Remove the owner package(s) of the repo file
			libdnf5::rpm::PackageQuery owner_query(*priv->base);
			owner_query.filter_installed();
			owner_query.filter_file({repo_file});
			for (auto pkg : owner_query) {
				goal.add_remove(pkg.get_full_nevra());
			}
			
			// If autoremove is true, also remove packages installed from these repos
			if (autoremove) {
				libdnf5::rpm::PackageQuery inst_query(*priv->base);
				inst_query.filter_installed();
				for (auto pkg : inst_query) {
					std::string from_repo = pkg.get_from_repo_id();
					for (const auto &id : all_repo_ids) {
						if (from_repo == id) {
							goal.add_remove(pkg.get_full_nevra());
							break;
						}
					}
				}
				// Also enable unused dependency removal
				priv->base->get_config().get_clean_requirements_on_remove_option().set(true);
			}
			
			pk_backend_job_set_status (job, PK_STATUS_ENUM_QUERY);
			auto trans = goal.resolve();
			if (!trans.get_transaction_problems().empty()) {
				std::string msg;
				for (const auto &p : trans.get_transaction_problems()) msg += p + "; ";
				pk_backend_job_error_code (job, PK_ERROR_ENUM_DEP_RESOLUTION_FAILED, "%s", msg.c_str());
				pk_backend_job_finished (job);
				return;
			}
			
			if (pk_bitfield_contain (transaction_flags, PK_TRANSACTION_FLAG_ENUM_SIMULATE)) {
				for (const auto &item : trans.get_transaction_packages()) {
					dnf5_emit_pkg(job, item.get_package());
				}
			} else {
				pk_backend_job_set_status (job, PK_STATUS_ENUM_RUNNING);
				trans.run();
				dnf5_setup_base (priv);
			}
		}
	} catch (const std::exception &e) {
		pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
	}
	pk_backend_job_finished (job);
}

// Backend API Implementation

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
	return pk_bitfield_from_enums (
		PK_ROLE_ENUM_DEPENDS_ON,
		PK_ROLE_ENUM_DOWNLOAD_PACKAGES,
		PK_ROLE_ENUM_GET_DETAILS,
		PK_ROLE_ENUM_GET_DETAILS_LOCAL,
		PK_ROLE_ENUM_GET_FILES,
		PK_ROLE_ENUM_GET_FILES_LOCAL,
		PK_ROLE_ENUM_GET_PACKAGES,
		PK_ROLE_ENUM_GET_REPO_LIST,
		PK_ROLE_ENUM_INSTALL_FILES,
		PK_ROLE_ENUM_INSTALL_PACKAGES,
		PK_ROLE_ENUM_REMOVE_PACKAGES,
		PK_ROLE_ENUM_UPDATE_PACKAGES,
		PK_ROLE_ENUM_REPAIR_SYSTEM,
		PK_ROLE_ENUM_UPGRADE_SYSTEM,
		PK_ROLE_ENUM_REPO_ENABLE,
		PK_ROLE_ENUM_REPO_REMOVE,
		PK_ROLE_ENUM_REPO_SET_DATA,
		PK_ROLE_ENUM_REQUIRED_BY,
		PK_ROLE_ENUM_RESOLVE,
		PK_ROLE_ENUM_REFRESH_CACHE,
		PK_ROLE_ENUM_GET_UPDATES,
		PK_ROLE_ENUM_GET_UPDATE_DETAIL,
		PK_ROLE_ENUM_WHAT_PROVIDES,
		PK_ROLE_ENUM_SEARCH_NAME,
		PK_ROLE_ENUM_SEARCH_DETAILS,
		PK_ROLE_ENUM_SEARCH_FILE,
		PK_ROLE_ENUM_CANCEL,
		-1);
}

void
pk_backend_initialize (GKeyFile *conf, PkBackend *backend)
{
	PkBackendDnf5Private *priv = g_new0 (PkBackendDnf5Private, 1);
	g_mutex_init (&priv->mutex);
	try {
		dnf5_setup_base (priv);
	} catch (const std::exception &e) {
		g_warning ("Init failed: %s", e.what());
	}
	pk_backend_set_user_data (backend, priv);
}

void
pk_backend_destroy (PkBackend *backend)
{
	PkBackendDnf5Private *priv = (PkBackendDnf5Private *) pk_backend_get_user_data (backend);
	priv->base.reset();
	g_mutex_clear (&priv->mutex);
	g_free (priv);
}

void
pk_backend_start_job (PkBackend *backend, PkBackendJob *job)
{
}

void
pk_backend_stop_job (PkBackend *backend, PkBackendJob *job)
{
}

void
pk_backend_search_names (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t^as)", filters, values), NULL);
}

void
pk_backend_search_details (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t^as)", filters, values), NULL);
}

void
pk_backend_search_files (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t^as)", filters, values), NULL);
}

void
pk_backend_get_packages (PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t)", filters), NULL);
}

void
pk_backend_resolve (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **package_ids)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t^as)", filters, package_ids), NULL);
}

void
pk_backend_get_details (PkBackend *backend, PkBackendJob *job, gchar **package_ids)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(^as)", package_ids), NULL);
}

void
pk_backend_get_files (PkBackend *backend, PkBackendJob *job, gchar **package_ids)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(^as)", package_ids), NULL);
}

void
pk_backend_get_repo_list (PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t)", filters), NULL);
}

void
pk_backend_get_updates (PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t)", filters), NULL);
}

void
pk_backend_what_provides (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **search)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t^as)", filters, search), NULL);
}

void
pk_backend_depends_on (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **package_ids, gboolean recursive)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t^asb)", filters, package_ids, recursive), NULL);
}

void
pk_backend_required_by (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **package_ids, gboolean recursive)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(t^asb)", filters, package_ids, recursive), NULL);
}

void
pk_backend_get_update_detail (PkBackend *backend, PkBackendJob *job, gchar **package_ids)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(^as)", package_ids), NULL);
}

void
pk_backend_download_packages (PkBackend *backend, PkBackendJob *job, gchar **package_ids, const gchar *directory)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(^as&s)", package_ids, directory), NULL);
}

void
pk_backend_get_details_local (PkBackend *backend, PkBackendJob *job, gchar **files)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(^as)", files), NULL);
}

void
pk_backend_get_files_local (PkBackend *backend, PkBackendJob *job, gchar **files)
{
	pk_backend_job_thread_create (job, dnf5_query_thread, g_variant_new ("(^as)", files), NULL);
}

void
pk_backend_install_packages (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **package_ids)
{
	pk_backend_job_thread_create (job, dnf5_transaction_thread, g_variant_new ("(t^as)", transaction_flags, package_ids), NULL);
}

void
pk_backend_remove_packages (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **package_ids, gboolean allow_deps, gboolean autoremove)
{
	pk_backend_job_thread_create (job, dnf5_transaction_thread, g_variant_new ("(t^asbb)", transaction_flags, package_ids, allow_deps, autoremove), NULL);
}

void
pk_backend_update_packages (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **package_ids)
{
	pk_backend_job_thread_create (job, dnf5_transaction_thread, g_variant_new ("(t^as)", transaction_flags, package_ids), NULL);
}

void
pk_backend_install_files (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **full_paths)
{
	pk_backend_job_thread_create (job, dnf5_transaction_thread, g_variant_new ("(t^as)", transaction_flags, full_paths), NULL);
}

void
pk_backend_upgrade_system (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, const gchar *distro_id, PkUpgradeKindEnum upgrade_kind)
{
	pk_backend_job_thread_create (job, dnf5_transaction_thread, g_variant_new ("(t&su)", transaction_flags, distro_id, upgrade_kind), NULL);
}

void
pk_backend_repair_system (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags)
{
	pk_backend_job_thread_create (job, dnf5_transaction_thread, g_variant_new ("(t)", transaction_flags), NULL);
}

void
pk_backend_repo_enable (PkBackend *backend, PkBackendJob *job, const gchar *repo_id, gboolean enabled)
{
	pk_backend_job_thread_create (job, dnf5_repo_thread, g_variant_new ("(sb)", repo_id, enabled), NULL);
}

void
pk_backend_repo_set_data (PkBackend *backend, PkBackendJob *job, const gchar *repo_id, const gchar *parameter, const gchar *value)
{
	pk_backend_job_thread_create (job, dnf5_repo_thread, g_variant_new ("(sss)", repo_id, parameter, value), NULL);
}

void
pk_backend_repo_remove (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, const gchar *repo_id, gboolean autoremove)
{
	pk_backend_job_thread_create (job, dnf5_repo_thread, g_variant_new ("(t&sb)", transaction_flags, repo_id, autoremove), NULL);
}

void
pk_backend_refresh_cache (PkBackend *backend, PkBackendJob *job, gboolean force)
{
	pk_backend_job_set_status (job, PK_STATUS_ENUM_REFRESH_CACHE);
	PkBackendDnf5Private *priv = (PkBackendDnf5Private *) pk_backend_get_user_data (backend);
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->mutex);
	try {
		dnf5_setup_base (priv);
	} catch (const std::exception &e) {
		pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s", e.what());
	}
	pk_backend_job_finished (job);
}

void
pk_backend_cancel (PkBackend *backend, PkBackendJob *job)
{
}

}
