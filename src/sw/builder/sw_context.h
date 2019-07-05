// Copyright (C) 2016-2019 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "concurrent_map.h"
#include "os.h"

#include <sw/manager/sw_context.h>

#include <shared_mutex>

struct Executor;

namespace sw
{

struct CommandStorage;
struct FileData;
struct FileStorage;
struct ProgramVersionStorage;

struct SW_BUILDER_API SwBuilderContext : SwManagerContext
{
    using FileDataHashMap = ConcurrentHashMap<path, FileData>;

    OS HostOS;

    SwBuilderContext(const path &local_storage_root_dir);
    virtual ~SwBuilderContext();

    ProgramVersionStorage &getVersionStorage() const;
    FileStorage &getFileStorage(const String &config, bool local) const;
    FileStorage &getServiceFileStorage() const;
    Executor &getFileStorageExecutor() const;
    FileDataHashMap &getFileData() const;
    CommandStorage &getCommandStorage() const;

    void clearFileStorages();

private:
    // keep order
    std::unique_ptr<ProgramVersionStorage> pvs;
    std::unique_ptr<CommandStorage> cs;
    std::unique_ptr<FileDataHashMap> fshm; // before FileStorages!
    mutable std::unique_ptr<FileStorage> file_storage;
    std::unique_ptr<Executor> file_storage_executor; // after everything!
};

} // namespace sw
