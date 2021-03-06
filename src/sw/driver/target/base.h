// Copyright (C) 2017-2018 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "enums.h"
#include "../build_settings.h"
#include "../program_storage.h"
#include "../license.h"
#include "../dependency.h"
#include "../types.h"
#include "../source_file.h"

#include <sw/builder/node.h>
#include <sw/builder/os.h>
#include <sw/manager/package.h>
#include <sw/manager/package_path.h>
#include <sw/manager/package_version_map.h>
#include <sw/manager/source.h>

#include <any>
#include <mutex>
#include <optional>

#include "base_macro.h"

namespace sw
{

SW_DRIVER_CPP_API
bool isExecutable(TargetType T);

SW_DRIVER_CPP_API
String toString(TargetType T);

struct NativeCompiledTarget;
struct Build;
struct SwContext;
struct SwBuild;
struct Target;
struct ProjectTarget;
struct DirectoryTarget;
using TargetBaseType = Target;
using TargetBaseTypePtr = std::shared_ptr<TargetBaseType>;
//using TargetBaseTypePtr = ITargetPtr;

struct ExecutableTarget;
struct LibraryTarget;
struct StaticLibraryTarget;
struct SharedLibraryTarget;

struct Test;

struct SW_DRIVER_CPP_API TargetEvent
{
    CallbackType t;
    std::function<void()> cb;
};

struct SW_DRIVER_CPP_API TargetEvents
{
    void add(CallbackType t, const std::function<void()> &cb);
    void call(CallbackType t) const;

private:
    std::vector<TargetEvent> events;
};

struct SW_DRIVER_CPP_API TargetBaseData : ProjectDirectories, TargetEvents
{
    bool DryRun = false;
    PackagePath NamePrefix;
    std::optional<CommandStorage *> command_storage;

    /**
     * \brief Target scope.
     */
    TargetScope Scope = TargetScope::Build;

    SwBuild &getMainBuild() const;

protected:
    const Build *build = nullptr;
    SwBuild *main_build_ = nullptr;

    // projects and dirs go here
    std::vector<TargetBaseTypePtr> dummy_children;
    std::optional<PackageId> current_project;
};

struct SW_DRIVER_CPP_API TargetBase : TargetBaseData
{
    TargetBase();
    virtual ~TargetBase();

    /**
    * \brief Add child target.
    */
    template <typename T, typename ... Args>
    T& add(const PackagePath &Name, Args && ... args)
    {
        if constexpr (sizeof...(Args) > 0)
        {
            if constexpr (std::is_convertible_v<std::tuple_element_t<0, std::tuple<Args...>>, Version>)
                return addTarget1<T>(Name, std::forward<Args>(args)...);
            else
                return addTarget1<T>(Name, pkg ? getPackage().getVersion() : Version(), std::forward<Args>(args)...);
        }
        else
            return addTarget1<T>(Name, pkg ? getPackage().getVersion() : Version(), std::forward<Args>(args)...);
    }

    /**
    * \brief Add child target.
    */
    template <typename T, typename ... Args>
    T &addTarget(Args && ... args)
    {
        return add<T>(std::forward<Args>(args)...);
    }

    /**
    * \brief Make target.
    */
    template <typename T, typename ... Args>
    std::shared_ptr<T> make(const PackagePath &Name, Args && ... args)
    {
        if constexpr (sizeof...(Args) > 0)
        {
            if constexpr (std::is_convertible_v<std::tuple_element_t<0, std::tuple<Args...>>, Version>)
                return makeTarget1<T>(Name, std::forward<Args>(args)...);
            else
                return makeTarget1<T>(Name, pkg ? getPackage().getVersion() : Version(), std::forward<Args>(args)...);
        }
        else
            return makeTarget1<T>(Name, pkg ? getPackage().getVersion() : Version(), std::forward<Args>(args)...);
    }

    /**
    * \brief Make target.
    */
    template <typename T, typename ... Args>
    std::shared_ptr<T> makeTarget(Args && ... args)
    {
        return make<T>(std::forward<Args>(args)...);
    }

    template <typename T>
    T &registerTarget(const std::shared_ptr<T> &t)
    {
        addChild(t);
        return *t;
    }

#define ADD_TARGET(t)                                       \
    template <typename... Args>                             \
    t##Target &add##t(Args &&... args)                      \
    {                                                       \
        return add<t##Target>(std::forward<Args>(args)...); \
    }
    // remove?
    ADD_TARGET(Executable)
    ADD_TARGET(Library)
    ADD_TARGET(StaticLibrary)
    ADD_TARGET(SharedLibrary)
#undef ADD_TARGET

    template <typename ... Args>
    ProjectTarget &addProject(Args && ... args) { return addTarget<ProjectTarget>(std::forward<Args>(args)...); }
    DirectoryTarget &addDirectory(const PackagePath &Name) { return addTarget<DirectoryTarget>(Name); }

    bool isLocal() const;

    Build &getSolution();
    const Build &getSolution() const;
    const SwContext &getContext() const;

protected:
    // impl
    bool prepared = false;

    TargetBase(const TargetBase &);

    LocalPackage &getPackageMutable();
    const LocalPackage &getPackage() const;

private:
    std::unique_ptr<LocalPackage> pkg;
    bool Local = true; // local projects

    template <typename T, typename ... Args>
    auto createTarget(Args && ... args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }

    template <typename T, typename ... Args>
    T &addTarget1(const PackagePath &Name, const Version &V, Args && ... args)
    {
        static_assert(std::is_base_of_v<Target, T>, "Provide a valid Target type.");

        auto t = createTarget<T>(std::forward<Args>(args)...);
        addTarget2(true, t, Name, V);
        return *t;
    }

    template <typename T, typename ... Args>
    std::shared_ptr<T> makeTarget1(const PackagePath &Name, const Version &V, Args && ... args)
    {
        static_assert(std::is_base_of_v<Target, T>, "Provide a valid Target type.");

        auto t = createTarget<T>(std::forward<Args>(args)...);
        addTarget2(false, t, Name, V);
        return t;
    }

    TargetBase &addTarget2(bool add, const TargetBaseTypePtr &t, const PackagePath &Name, const Version &V);

    PackagePath constructTargetName(const PackagePath &Name) const;
    TargetBase &addChild(const TargetBaseTypePtr &t);
    virtual void setupTarget(TargetBaseType *t) const;

    friend struct Assigner;
};

struct SW_DRIVER_CPP_API TargetDescription
{
    LicenseType License = LicenseType::UnspecifiedOpenSource;
    path LicenseFilename;

    std::string fullname;
    std::string description;
    std::string url;
    std::string bugreport_url;
    std::string email;
    // build, test emails?
    /**
    * \brief Where to find this target.
    * - on site
    * - in store?
    */
    PackagePath category; // lowercase only!
    StringSet tags; // lowercase only!
                    // changes-file
                    // description-file (or readme file)
};

/**
* \brief Single project target.
*/
struct SW_DRIVER_CPP_API Target : ITarget, TargetBase, ProgramStorage,
    std::enable_shared_from_this<Target>
{
    /*struct TargetSettings
    {
        BuildSettings ss;
        std::set<PackageId> dependencies;
        StringSet features; // make map with values?
    };
    const TargetSettings *ts = nullptr;*/

    // Data storage for objects that must be alive with the target.
    // For example, program clones etc.
    std::vector<std::any> Storage;

    // rename to information?
    TargetDescription Description; // or inherit?
    std::optional<bool> Publish;
    bool AllowEmptyRegexes = false;

    // inheritable, move to native? what about other langs?
    //std::vector<TargetDependency> tdeps;
    // always not inheritable
    std::vector<DependencyPtr> DummyDependencies; // host config, but allowing some changes (configuration type/mt)
    std::vector<DependencyPtr> SourceDependencies; // no config, dependency on source files
    // build dir deps?
    std::vector<DependencyPtr> RuntimeDependencies; // this target config

    DependencyPtr addDummyDependency(const Target &);
    DependencyPtr addDummyDependency(const DependencyPtr &);
private:
    void addSourceDependency(const Target &);
    void addSourceDependency(const DependencyPtr &);
public:
    //

    using TargetBase::TargetBase;
    Target() = default;
    virtual ~Target() = default;

    // api
    const LocalPackage &getPackage() const override { return TargetBase::getPackage(); }
    const Source &getSource() const override;
    Files getSourceFiles() const override;
    std::vector<IDependency *> getDependencies() const override;
    const TargetSettings &getSettings() const override;
    const TargetSettings &getInterfaceSettings() const override;

    const TargetSettings &getTargetSettings() const { return getSettings(); }
    const BuildSettings &getBuildSettings() const;

    TargetSettings &getOptions();
    const TargetSettings &getOptions() const;
    TargetSettings &getExportOptions();
    const TargetSettings &getExportOptions() const;

    //
    Commands getCommands() const override;
    UnresolvedDependenciesType gatherUnresolvedDependencies() const;
    DependencyPtr getDependency() const; // returns current target as dependency
    void registerCommand(builder::Command &cmd);

    String getConfig() const; // make api?

    Target &operator+=(const Source &);
    Target &operator+=(std::unique_ptr<Source>);
    void operator=(const Source &); // remove?
    void setSource(const Source &);
    void fetch();

    FileStorage &getFs() const;

    path getLocalOutputBinariesDirectory() const;
    path getTargetDirShort(const path &root) const;

    void setRootDirectory(const path &);

    // main apis
    virtual bool init(); // multipass init
    virtual bool prepare() override { return false; } // multipass prepare
    virtual Files gatherAllFiles() const { return {}; }
    virtual DependenciesType gatherDependencies() const { return DependenciesType{}; }

    // other
    virtual void removeFile(const path &fn, bool binary_dir = false);

    //auto getPreparePass() const { return prepare_pass; }
    virtual bool mustResolveDeps() const { return deps_resolved ? false : (deps_resolved = true); }

    Program *findProgramByExtension(const String &ext) const;

    // using in build, move to protected when not used
    path getObjectDir(const LocalPackage &pkg) const;
    static path getObjectDir(const LocalPackage &pkg, const String &cfg);

    // from other target
    path getFile(const DependencyPtr &dep, const path &fn = {});
    path getFile(const Target &dep, const path &fn = {});

    //
    virtual TargetType getType() const { return TargetType::Unspecified; }
    String getTypeName() const { return toString(getType()); }
    bool hasSameProject(const ITarget &t) const;

    // tests
    // TODO: implement some of https://cmake.org/cmake/help/latest/manual/cmake-properties.7.html#properties-on-tests
    Test addTest();
    Test addTest(const String &name);
    Test addTest(const String &name, const ExecutableTarget &t);
    Test addTest(const ExecutableTarget &t);

private:
    void addTest(Test &cb, const String &name);
    String getTestName(const String &name = {}) const;

protected:
    path RootDirectory;
    SW_MULTIPASS_VARIABLE(prepare_pass);
    SW_MULTIPASS_VARIABLE(init_pass);
    mutable bool deps_resolved = false;
    mutable TargetSettings interface_settings;

    //Target(const Target &);
    CommandStorage *getCommandStorage() const;

private:
    TargetSettings ts; // this settings
    // export settings may be different
    // example: we set 'static-deps' setting which changes
    // ["native"]["library"] to "static";
    TargetSettings ts_export;
    BuildSettings bs;
    std::unique_ptr<Source> source;
    String provided_cfg;
    Commands tests;

    void applyRootDirectory();
    const TargetSettings &getHostSettings() const;

    virtual Commands getCommands1() const { return Commands{}; }
    Commands getTests() const override { return tests; }

    // for source access
    friend struct TargetBase;
};

struct SW_DRIVER_CPP_API ProjDirBase : Target
{
    using Target::Target;
    using Target::operator=;

    virtual ~ProjDirBase() = default;

    TargetType getType() const override { return TargetType::Directory; }
};

struct SW_DRIVER_CPP_API DirectoryTarget : ProjDirBase
{
    using ProjDirBase::ProjDirBase;
    virtual ~DirectoryTarget() = default;
};

struct SW_DRIVER_CPP_API ProjectTarget : ProjDirBase
{
    using ProjDirBase::ProjDirBase;
    virtual ~ProjectTarget() = default;

    bool init() override;
    TargetType getType() const override { return TargetType::Project; }
};

//template <class ... Args>
//struct SW_DRIVER_CPP_API TargetOptions : Args...
struct SW_DRIVER_CPP_API TargetOptions : SourceFileStorage, NativeOptions
{
    using SourceFileStorage::add;
    using SourceFileStorage::remove;
    using SourceFileStorage::operator=;

    using NativeOptions::add;
    using NativeOptions::remove;
    using NativeOptions::operator=;

    void add(const IncludeDirectory &);
    void remove(const IncludeDirectory &);

    void add(const LinkDirectory &);
    void remove(const LinkDirectory &);

    void add(const PrecompiledHeader &);
    void remove(const PrecompiledHeader &);

    void add(const Framework &);
    void remove(const Framework &);

    void merge(const TargetOptions &g, const GroupSettings &s = GroupSettings())
    {
        SourceFileStorage::merge(g, s);
        NativeOptions::merge(g, s);
    }

private:
    ASSIGN_WRAPPER(add, TargetOptions);
    ASSIGN_WRAPPER(remove, TargetOptions);
    ASSIGN_WRAPPER(remove_exclude, TargetOptions);

public:
    // source files
    //ASSIGN_TYPES(String)
    ASSIGN_TYPES_AND_EXCLUDE(path)
    ASSIGN_TYPES_AND_EXCLUDE(Files)
    ASSIGN_TYPES_AND_EXCLUDE(FileRegex)

    // compiler options
    ASSIGN_TYPES(Definition)
    ASSIGN_TYPES(DefinitionsType)
    ASSIGN_TYPES(IncludeDirectory)
    ASSIGN_TYPES(PrecompiledHeader)
    ASSIGN_TYPES(Framework)

    // linker options
    ASSIGN_TYPES(LinkDirectory)
    ASSIGN_TYPES(LinkLibrary)
    ASSIGN_TYPES(SystemLinkLibrary)

    //
    ASSIGN_TYPES(Target)
    ASSIGN_TYPES(PackageId)
    ASSIGN_TYPES(DependencyPtr)
    ASSIGN_TYPES(UnresolvedPackage)
    ASSIGN_TYPES(UnresolvedPackages)

    //
    ASSIGN_TYPES(sw::tag_static_t)
    ASSIGN_TYPES(sw::tag_shared_t)
};

template <class T>
struct SW_DRIVER_CPP_API TargetOptionsGroup :
    InheritanceGroup<T>
{
    using Base = InheritanceGroup<T>;

private:
    ASSIGN_WRAPPER(add, TargetOptionsGroup);
    ASSIGN_WRAPPER(remove, TargetOptionsGroup);

public:
    SW_TARGET_USING_ASSIGN_OPS(TargetOptions);
};

struct SW_DRIVER_CPP_API NativeTargetOptionsGroup : TargetOptionsGroup<TargetOptions>
{
    SW_TARGET_USING_ASSIGN_OPS(TargetOptionsGroup<TargetOptions>);

private:
    ASSIGN_WRAPPER(add, NativeTargetOptionsGroup);
    ASSIGN_WRAPPER(remove, NativeTargetOptionsGroup);

public:
    VariablesType Variables;
    ASSIGN_TYPES(Variable)

    void add(const Variable &v);
    void remove(const Variable &v);

    Files gatherAllFiles() const;
    DependenciesType gatherDependencies() const;
};

template <class SF>
std::unordered_set<SF*> gatherSourceFiles(const SourceFileStorage &s, const StringSet &exts = {})
{
    // maybe cache result?
    std::unordered_set<SF*> files;
    for (auto &[p, f] : s)
    {
        if (!f->isActive())
            continue;
        if (!exts.empty() && exts.find(p.extension().string()) == exts.end())
            continue;
        if (auto f2 = f->template as<SF*>())
            files.insert(f2);
    }
    return files;
}

path getOutputFileName(const Target &t);
path getBaseOutputDirNameForLocalOnly(const Target &t, const path &root, const path &OutputDir);
path getBaseOutputDirName(const Target &t, const path &OutputDir, const path &subdir);
path getBaseOutputFileNameForLocalOnly(const Target &t, const path &root, const path &OutputDir);
path getBaseOutputFileName(const Target &t, const path &OutputDir, const path &subdir);

} // namespace sw
