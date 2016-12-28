/*
 * Copyright (c) 2016, Egor Pugin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *     2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *     3. Neither the name of the copyright holder nor the names of
 *        its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "resolver.h"

#include "access_table.h"
#include "config.h"
#include "database.h"
#include "directories.h"
#include "exceptions.h"
#include "executor.h"
#include "hash.h"
#include "hasher.h"
#include "lock.h"
#include "pack.h"
#include "project.h"
#include "settings.h"
#include "sqlite_database.h"
#include "templates.h"
#include "verifier.h"

#include <boost/algorithm/string.hpp>

#include "logger.h"
DECLARE_STATIC_LOGGER(logger, "resolver");

#define CURRENT_API_LEVEL 1

TYPED_EXCEPTION(LocalDbHashException);
TYPED_EXCEPTION(DependencyNotResolved);

Resolver::Dependencies getDependenciesFromRemote(const Packages &deps, const Remote *current_remote);
Resolver::Dependencies getDependenciesFromDb(const Packages &deps, const Remote *current_remote);
Resolver::Dependencies prepareIdDependencies(const IdDependencies &id_deps, const Remote *current_remote);

void resolve_dependencies(const Packages &deps)
{
    Resolver r;
    r.resolve_dependencies(deps);
}

void resolve_and_download(const Package &p, const path &fn)
{
    Resolver r;
    r.resolve_and_download(p, fn);
}

void Resolver::resolve_dependencies(const Packages &dependencies)
{
    Packages deps;

    // remove some packages
    for (auto &d : dependencies)
    {
        // remove local packages
        if (d.second.ppath.is_loc())
            continue;

        // remove already downloaded packages
        auto i = rd.resolved_packages.find(d.second);
        if (i != rd.resolved_packages.end())
            continue;

        deps.insert(d);
    }

    if (deps.empty())
        return;

    resolve(deps, [this] { download_and_unpack(); });

    // mark packages as resolved
    for (auto &d : deps)
        rd.resolved_packages.insert(d.second);

    // other related stuff
    read_configs();
    post_download();
}

void Resolver::resolve_and_download(const Package &p, const path &fn)
{
    resolve({ { p.ppath.toString(), p } }, [&]
    {
        for (auto &dd : download_dependencies_)
        {
            if (dd.second == p)
            {
                download(dd.second, fn);
                break;
            }
        }
    });
}

void Resolver::resolve(const Packages &deps, std::function<void()> resolve_action)
{
    if (!resolve_action)
        throw std::logic_error("Empty resolve action!");

    // ref to not invalidate all ptrs
    auto &us = Settings::get_user_settings();
    auto cr = us.remotes.begin();
    current_remote = &*cr++;

    auto resolve_remote_deps = [this, &deps, &cr, &us]()
    {
        bool again = true;
        while (again)
        {
            again = false;
            try
            {
                LOG_INFO(logger, "Trying " + current_remote->name + " remote");
                download_dependencies_ = getDependenciesFromRemote(deps, current_remote);
            }
            catch (const std::exception &e)
            {
                LOG_WARN(logger, e.what());
                if (cr != us.remotes.end())
                {
                    current_remote = &*cr++;
                    again = true;
                }
                else
                    throw DependencyNotResolved();
            }
        }
    };

    query_local_db = !us.force_server_query;
    // do 2 attempts: 1) local db, 2) remote db
    int n_attempts = query_local_db ? 2 : 1;
    while (n_attempts--)
    {
        try
        {
            if (query_local_db)
            {
                try
                {
                    download_dependencies_ = getDependenciesFromDb(deps, current_remote);
                }
                catch (std::exception &e)
                {
                    LOG_ERROR(logger, "Cannot get dependencies from local database: " << e.what());

                    query_local_db = false;
                    resolve_remote_deps();
                }
            }
            else
            {
                resolve_remote_deps();
            }

            resolve_action();
        }
        catch (LocalDbHashException &)
        {
            LOG_WARN(logger, "Local db data caused issues, trying remote one");

            query_local_db = false;
            continue;
        }
        break;
    }
}

void Resolver::download(const DownloadDependency &d, const path &fn)
{
    if (!d.remote->downloadPackage(d, d.sha256, fn, query_local_db))
    {
        // if we get hashes from local db
        // they can be stalled within server refresh time (15 mins)
        // in this case we should do request to server
        auto err = "Hashes do not match for package: " + d.target_name;
        if (query_local_db)
            throw LocalDbHashException(err);
        throw std::runtime_error(err);
    }
}

void Resolver::download_and_unpack()
{
    if (download_dependencies_.empty())
        return;

    auto download_dependency = [this](auto &dd)
    {
        auto &d = dd.second;
        auto version_dir = d.getDirSrc();
        auto hash_file = d.getStampFilename();
        bool must_download = d.getStampHash() != d.sha256 || d.sha256.empty();

        if (fs::exists(version_dir) && !must_download)
            return;

        // lock, so only one cppan process at the time could download the project
        ScopedFileLock lck(hash_file, std::defer_lock);
        if (!lck.try_lock())
        {
            // download is in progress, wait and register config
            ScopedFileLock lck2(hash_file);
            rd.add_config(d);
            return;
        }

        // verify before any actions, so stop on error
        if (Settings::get_local_settings().verify_all)
            verify(d);

        // remove existing version dir
        cleanPackages(d.target_name);

        // dl
        LOG_INFO(logger, "Downloading: " << d.target_name << "...");

        // maybe d.target_name instead of version_dir.string()?
        path fn = make_archive_name(version_dir.string());
        download(d, fn);

        rd.downloads++;
        write_file(hash_file, d.sha256);

        LOG_INFO(logger, "Unpacking  : " << d.target_name << "...");
        Files files;
        try
        {
            files = unpack_file(fn, version_dir);
        }
        catch (...)
        {
            fs::remove_all(version_dir);
            throw;
        }
        fs::remove(fn);

        // re-read in any case
        // no need to remove old config, let it die with program
        auto c = rd.add_config(d);

        // move all files under unpack dir
        auto ud = c->getDefaultProject(d.ppath).unpack_directory;
        if (!ud.empty())
        {
            ud = version_dir / ud;
            if (fs::exists(ud))
                throw std::runtime_error("Cannot create unpack_directory '" + ud.string() + "' because fs object with the same name alreasy exists");
            fs::create_directories(ud);
            for (auto &f : boost::make_iterator_range(fs::directory_iterator(version_dir), {}))
            {
                if (f == ud || f.path().filename() == CPPAN_FILENAME)
                    continue;
                if (fs::is_directory(f))
                {
                    copy_dir(f, ud / f.path().filename());
                    fs::remove_all(f);
                }
                else if (fs::is_regular_file(f))
                {
                    fs::copy_file(f, ud / f.path().filename());
                    fs::remove(f);
                }
            }
        }
    };

    Executor e(get_max_threads(8));
    e.throw_exceptions = true;

    // threaded execution does not preserve object creation/destruction order,
    // so current path is not correctly restored
    ScopedCurrentPath cp;

    for (auto &dd : download_dependencies_)
        e.push([&download_dependency, &dd] { download_dependency(dd); });

    e.wait();

    // two following blocks use executor to do parallel queries
    if (query_local_db)
    {
        // send download list
        // remove this when cppan will be widely used
        // also because this download count can be easily abused
        e.push([this]()
        {
            if (!current_remote)
                return;

            ptree request;
            ptree children;
            for (auto &d : download_dependencies_)
            {
                ptree c;
                c.put("", d.second.id);
                children.push_back(std::make_pair("", c));
            }
            request.add_child("vids", children);

            try
            {
                HttpRequest req = httpSettings;
                req.type = HttpRequest::POST;
                req.url = current_remote->url + "/api/add_downloads";
                req.data = ptree2string(request);
                auto resp = url_request(req);
            }
            catch (...)
            {
            }
        });
    }

    // send download action once
    RUN_ONCE
    {
        e.push([this]
        {
            try
            {
                HttpRequest req = httpSettings;
                req.type = HttpRequest::POST;
                req.url = current_remote->url + "/api/add_client_call";
                req.data = "{}"; // empty json
                auto resp = url_request(req);
            }
            catch (...)
            {
            }
        });
    };

    e.wait();
}

void Resolver::post_download()
{
    for (auto &cc : rd)
        prepare_config(cc);
}

void Resolver::prepare_config(PackageStore::PackageConfigs::value_type &cc)
{
    auto &p = cc.first;
    auto &c = cc.second.config;
    auto &dependencies = cc.second.dependencies;
    c->setPackage(p);
    auto &project = c->getDefaultProject(p.ppath);

    if (p.flags[pfLocalProject])
        return;

    // prepare deps: extract real deps flags from configs
    for (auto &dep : download_dependencies_[p].getDependencies())
    {
        auto d = dep.second;
        auto i = project.dependencies.find(d.ppath.toString());
        if (i == project.dependencies.end())
        {
            // check if we chose a root project that matches all subprojects
            Packages to_add;
            std::set<String> to_remove;
            for (auto &root_dep : project.dependencies)
            {
                for (auto &child_dep : download_dependencies_[p].getDependencies())
                {
                    if (root_dep.second.ppath.is_root_of(child_dep.second.ppath))
                    {
                        to_add.insert({ child_dep.second.ppath.toString(), child_dep.second });
                        to_remove.insert(root_dep.second.ppath.toString());
                    }
                }
            }
            if (to_add.empty())
                throw std::runtime_error("dependency '" + d.ppath.toString() + "' not found");
            for (auto &r : to_remove)
                project.dependencies.erase(r);
            for (auto &a : to_add)
                project.dependencies.insert(a);
            continue;
        }
        d.flags[pfIncludeDirectoriesOnly] = i->second.flags[pfIncludeDirectoriesOnly];
        i->second.version = d.version;
        i->second.flags = d.flags;
        dependencies.emplace(d.ppath.toString(), d);
    }

    c->post_download();
}

void Resolver::read_configs()
{
    if (download_dependencies_.empty())
        return;
    LOG_INFO(logger, "Reading package specs... ");
    for (auto &d : download_dependencies_)
        read_config(d.second);
}

void Resolver::read_config(const DownloadDependency &d)
{
    if (!fs::exists(d.getDirSrc()))
        return;

    try
    {
        auto p = rd.config_store.insert(std::make_unique<Config>(d.getDirSrc()));
        rd.packages[d].config = p.first->get();
    }
    catch (DependencyNotResolved &)
    {
        // do not swallow
        throw;
    }
    catch (std::exception &)
    {
        // something wrong, remove the whole dir to re-download it
        fs::remove_all(d.getDirSrc());

        // but do not swallow
        throw;
    }
}

void Resolver::assign_dependencies(const Package &pkg, const Packages &deps)
{
    rd.packages[pkg].dependencies.insert(deps.begin(), deps.end());
    for (auto &dd : download_dependencies_)
    {
        if (!dd.second.flags[pfDirectDependency])
            continue;
        auto &deps2 = rd.packages[pkg].dependencies;
        auto i = deps2.find(dd.second.ppath.toString());
        if (i == deps2.end())
        {
            // check if we chose a root project match all subprojects
            Packages to_add;
            std::set<String> to_remove;
            for (auto &root_dep : deps2)
            {
                for (auto &child_dep : download_dependencies_)
                {
                    if (root_dep.second.ppath.is_root_of(child_dep.second.ppath))
                    {
                        to_add.insert({ child_dep.second.ppath.toString(), child_dep.second });
                        to_remove.insert(root_dep.second.ppath.toString());
                    }
                }
            }
            if (to_add.empty())
                throw std::runtime_error("cannot match dependency");
            for (auto &r : to_remove)
                deps2.erase(r);
            for (auto &a : to_add)
                deps2.insert(a);
            continue;
        }
        auto &d = i->second;
        d.version = dd.second.version;
        d.flags |= dd.second.flags;
        d.createNames();
    }
}

Resolver::Dependencies getDependenciesFromRemote(const Packages &deps, const Remote *current_remote)
{
    // prepare request
    ptree request;
    ptree dependency_tree;
    for (auto &d : deps)
    {
        ptree version;
        version.put("version", d.second.version.toAnyVersion());
        request.put_child(ptree::path_type(d.second.ppath.toString(), '|'), version);
    }

    LOG_INFO(logger, "Requesting dependency list... ");
    {
        int ct = 5;
        int t = 10;
        int n_tries = 3;
        while (1)
        {
            HttpResponse resp;
            try
            {
                HttpRequest req = httpSettings;
                req.connect_timeout = ct;
                req.timeout = t;
                req.type = HttpRequest::POST;
                req.url = current_remote->url + "/api/find_dependencies";
                req.data = ptree2string(request);
                resp = url_request(req);
                if (resp.http_code != 200)
                    throw std::runtime_error("Cannot get deps");
                dependency_tree = string2ptree(resp.response);
                break;
            }
            catch (...)
            {
                if (--n_tries == 0)
                {
                    switch (resp.http_code)
                    {
                    case 200:
                    {
                        dependency_tree = string2ptree(resp.response);
                        auto e = dependency_tree.find("error");
                        LOG_WARN(logger, e->second.get_value<String>());
                    }
                    break;
                    case 0:
                        LOG_WARN(logger, "Could not connect to server");
                        break;
                    default:
                        LOG_WARN(logger, "Error code: " + std::to_string(resp.http_code));
                        break;
                    }
                    throw;
                }
                else if (resp.http_code == 0)
                {
                    ct /= 2;
                    t /= 2;
                }
                LOG_INFO(logger, "Retrying... ");
            }
        }
    }

    // read deps urls, download them, unpack
    int api = 0;
    if (dependency_tree.find("api") != dependency_tree.not_found())
        api = dependency_tree.get<int>("api");

    auto e = dependency_tree.find("error");
    if (e != dependency_tree.not_found())
        throw std::runtime_error(e->second.get_value<String>());

    auto info = dependency_tree.find("info");
    if (info != dependency_tree.not_found())
        LOG_INFO(logger, info->second.get_value<String>());

    if (api == 0)
        throw std::runtime_error("API version is missing in the response");
    if (api > CURRENT_API_LEVEL)
        throw std::runtime_error("Server uses more new API version. Please, upgrade the cppan client from site or via --self-upgrade");
    if (api < CURRENT_API_LEVEL - 1)
        throw std::runtime_error("Your client's API is newer than server's. Please, wait for server upgrade");

    // dependencies were received without error

    // set id dependencies
    IdDependencies id_deps;
    int unresolved = (int)deps.size();
    auto &remote_packages = dependency_tree.get_child("packages");
    for (auto &v : remote_packages)
    {
        auto id = v.second.get<ProjectVersionId>("id");

        DownloadDependency d;
        d.ppath = v.first;
        d.version = v.second.get<String>("version");
        d.flags = decltype(d.flags)(v.second.get<uint64_t>("flags"));
        d.sha256 = v.second.get<String>("sha256");

        if (v.second.find(DEPENDENCIES_NODE) != v.second.not_found())
        {
            std::set<ProjectVersionId> idx;
            for (auto &tree_dep : v.second.get_child(DEPENDENCIES_NODE))
                idx.insert(tree_dep.second.get_value<ProjectVersionId>());
            d.setDependencyIds(idx);
        }

        id_deps[id] = d;

        unresolved--;
    }

    if (unresolved > 0)
        throw std::runtime_error("Some packages (" + std::to_string(unresolved) + ") are unresolved");

    return prepareIdDependencies(id_deps, current_remote);
}

Resolver::Dependencies getDependenciesFromDb(const Packages &deps, const Remote *current_remote)
{
    auto &db = getPackagesDatabase();
    auto id_deps = db.findDependencies(deps);
    return prepareIdDependencies(id_deps, current_remote);
}

Resolver::Dependencies prepareIdDependencies(const IdDependencies &id_deps, const Remote *current_remote)
{
    Resolver::Dependencies dependencies;
    for (auto &v : id_deps)
    {
        auto d = v.second;
        d.createNames();
        d.remote = current_remote;
        d.prepareDependencies(id_deps);
        dependencies[d] = d;
    }
    return dependencies;
}
