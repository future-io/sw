// Copyright (C) 2016-2019 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "package.h"

#include <sw/support/filesystem.h>

#include <memory>
#include <mutex>
#include <vector>

namespace sw
{

struct IResolvableStorage;
struct Storage;
struct CachedStorage;
struct LocalStorage;

// sw_context_t?
/*struct SW_MANAGER_API ISwContext
{
    virtual ~ISwContext() = 0;

    virtual Package resolve(const UnresolvedPackage &) const = 0;
};*/

struct SW_MANAGER_API SwManagerContext// : ISwContext
{
    SwManagerContext(const path &local_storage_root_dir);
    virtual ~SwManagerContext();

    LocalStorage &getLocalStorage();
    const LocalStorage &getLocalStorage() const;
    std::vector<Storage *> getRemoteStorages();
    std::vector<const Storage *> getRemoteStorages() const;

    //
    std::unordered_map<UnresolvedPackage, LocalPackage> install(const UnresolvedPackages &, bool use_cache = true) const;
    LocalPackage install(const Package &) const;

    std::unordered_map<UnresolvedPackage, PackagePtr> resolve(const UnresolvedPackages &, bool use_cache = true) const;
    LocalPackage resolve(const UnresolvedPackage &) const;

    // lock file related
    void setCachedPackages(const std::unordered_map<UnresolvedPackage, PackageId> &) const;

private:
    int cache_storage_id;
    int local_storage_id;
    int first_remote_storage_id;
    std::vector<std::unique_ptr<IStorage>> storages;
    mutable std::mutex resolve_mutex;

    CachedStorage &getCachedStorage() const;
    std::unordered_map<UnresolvedPackage, PackagePtr> resolve(const UnresolvedPackages &, const std::vector<IStorage*> &) const;
};

} // namespace sw
