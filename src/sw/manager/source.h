// Copyright (C) 2016-2019 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <primitives/source.h>

namespace sw
{

inline namespace source
{

using primitives::source::Source;

using primitives::source::EmptySource;
using primitives::source::Hg;
using primitives::source::Mercurial;
using primitives::source::Bzr;
using primitives::source::Bazaar;
using primitives::source::Fossil;
using primitives::source::Cvs;
using primitives::source::Svn;
using primitives::source::RemoteFile;
using primitives::source::RemoteFiles;

struct SW_MANAGER_API Git : primitives::source::Git
{
    using primitives::source::Git::Git;

    Git(const String &url);
    Git(const Git &) = default;

    bool isValid();

private:
    std::unique_ptr<Source> clone() const override { return std::make_unique<Git>(*this); }
};

/// load from current (passed) object, detects 'getString()' subobject
SW_MANAGER_API
std::unique_ptr<Source> load(const nlohmann::json &j);

}

namespace detail
{

struct DownloadData
{
    path root_dir;
    path requested_dir;
    path stamp_file;

    path getRequestedDirectory() const { return requested_dir; }
    void remove() const;
};

}

using SourcePtr = std::unique_ptr<Source>;
using SourceDirMap = std::unordered_map<String, detail::DownloadData>;

struct SourceDownloadOptions
{
    path root_dir; // root to download
    bool ignore_existing_dirs = false;
    std::chrono::seconds existing_dirs_age{ 0 };
    bool adjust_root_dir = true;
};

// returns true if downloaded
SW_MANAGER_API
bool download(const std::unordered_set<SourcePtr> &sources, SourceDirMap &source_dirs, const SourceDownloadOptions &opts = {});

SW_MANAGER_API
SourceDirMap download(const std::unordered_set<SourcePtr> &sources, const SourceDownloadOptions &opts = {});

}
