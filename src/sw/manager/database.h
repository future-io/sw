// Copyright (C) 2016-2019 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "package_version_map.h"
#include "remote.h"

#include <sw/support/filesystem.h>

#include <primitives/date_time.h>
#include <optional>

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace sqlpp::sqlite3 { class connection; }

namespace sw
{

struct LocalStorage;
struct PackageId;

struct SW_MANAGER_API Database
{
    std::unique_ptr<sqlpp::sqlite3::connection> db;
    path fn;

    Database(const path &db_name, const String &schema);
    ~Database();

    void open(bool read_only = false, bool in_memory = false);

    int getIntValue(const String &key);
    void setIntValue(const String &key, int v);

protected:
    //
    template <typename T>
    std::optional<T> getValue(const String &key) const;

    template <typename T>
    T getValue(const String &key, const T &default_) const;

    template <typename T>
    void setValue(const String &key, const T &v) const;
};

struct SW_MANAGER_API PackagesDatabase : Database
{
    PackagesDatabase(const path &db_fn);
    ~PackagesDatabase();

    void open(bool read_only = false, bool in_memory = false);

    std::unordered_map<UnresolvedPackage, PackageId> resolve(const UnresolvedPackages &pkgs, UnresolvedPackages &unresolved_pkgs) const;

    PackageData getPackageData(const PackageId &) const;

    int64_t getInstalledPackageId(const PackageId &) const;
    String getInstalledPackageHash(const PackageId &) const;
    bool isPackageInstalled(const Package &) const;
    void installPackage(const Package &);
    void installPackage(const PackageId &, const PackageData &);
    void deletePackage(const PackageId &) const;
    PackageId getGroupLeader(PackageVersionGroupNumber gn) const;
    void setGroupNumber(const PackageId &, PackageVersionGroupNumber) const;

    // overridden
    std::optional<path> getOverriddenDir(const Package &p) const;
    std::unordered_set<PackageId> getOverriddenPackages() const;
    void deleteOverriddenPackageDir(const path &sdir) const;

    DataSources getDataSources() const;

    db::PackageId getPackageId(const PackagePath &) const;
    db::PackageId getPackageVersionId(const PackageId &) const;
    String getPackagePath(db::PackageId) const;

    std::vector<PackagePath> getMatchingPackages(const String &name = {}) const;
    std::vector<Version> getVersionsForPackage(const PackagePath &) const;

private:
    std::mutex m;
    std::unique_ptr<struct PreparedStatements> pps;
};

}
