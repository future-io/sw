// Copyright (C) 2016-2019 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "concurrent_map.h"
#include "module_storage.h"
#include "os.h"

#include <sw/manager/sw_context.h>

#include <shared_mutex>

struct Executor;

namespace sw
{

struct CommandStorage;
struct FileStorage;
struct ProgramVersionStorage;

namespace builder::detail { struct ResolvableCommand; }

struct SW_BUILDER_API SwBuilderContext : SwManagerContext
{
    OS HostOS;

    SwBuilderContext(const path &local_storage_root_dir);
    virtual ~SwBuilderContext();

    ProgramVersionStorage &getVersionStorage() const;
    FileStorage &getFileStorage() const;
    Executor &getFileStorageExecutor() const;
    CommandStorage &getCommandStorage(const path &root) const;
    ModuleStorage &getModuleStorage() const;
    const OS &getHostOs() const { return HostOS; }

    void clearFileStorages();

private:
    std::unique_ptr<ModuleStorage> module_storage;
    // keep order
    std::unique_ptr<ProgramVersionStorage> pvs;
    mutable std::unordered_map<path, std::unique_ptr<CommandStorage>> command_storages;
    mutable std::unique_ptr<FileStorage> file_storage;
    std::unique_ptr<Executor> file_storage_executor; // after everything!

    mutable std::mutex csm;
};

SW_BUILDER_API
Version getVersion(
    const SwBuilderContext &swctx, builder::detail::ResolvableCommand &c,
    const String &in_regex = {});

SW_BUILDER_API
Version getVersion(
    const SwBuilderContext &swctx, const path &program,
    const String &arg = "--version", const String &in_regex = {});

} // namespace sw
