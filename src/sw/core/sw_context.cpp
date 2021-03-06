// Copyright (C) 2016-2019 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "sw_context.h"

#include "build.h"
#include "input.h"
#include "driver.h"

#include <sw/manager/storage.h>

#include <primitives/log.h>
DECLARE_STATIC_LOGGER(logger, "context");

namespace sw
{

void detectCompilers(SwCoreContext &s);

IDriver::~IDriver() = default;

SwCoreContext::SwCoreContext(const path &local_storage_root_dir)
    : SwBuilderContext(local_storage_root_dir)
{
    detectCompilers(*this);

    host_settings = createHostSettings();

    LOG_TRACE(logger, "Host configuration: " + getHostSettings().toString());
}

SwCoreContext::~SwCoreContext()
{
}

TargetSettings SwCoreContext::createHostSettings() const
{
    auto ts = toTargetSettings(getHostOs());

#ifdef _WIN32
  #ifdef NDEBUG
    ts["native"]["configuration"] = "release";
  #else
    ts["native"]["configuration"] = "debug";
  #endif
#else
    ts["native"]["configuration"] = "release";
#endif
    ts["native"]["library"] = "shared";
    ts["native"]["mt"] = "false";

    // setHostPrograms()
    // after detection
    // move this to driver?
    ts["native"]["configuration"] = "release";
    ts["native"]["library"] = "shared";

    auto to_upkg = [](const auto &s)
    {
        return UnresolvedPackage(s).toString();
    };

    if (getHostOs().is(OSType::Windows))
    {
        ts["native"]["stdlib"]["c"] = to_upkg("com.Microsoft.Windows.SDK.ucrt");
        ts["native"]["stdlib"]["cpp"] = to_upkg("com.Microsoft.VisualStudio.VC.libcpp");
        ts["native"]["stdlib"]["kernel"] = to_upkg("com.Microsoft.Windows.SDK.um");

        // now find the latest available sdk (ucrt) and select it
        TargetSettings oss;
        oss["os"] = ts["os"];
        auto sdk = getPredefinedTargets().find(UnresolvedPackage(ts["native"]["stdlib"]["c"].getValue()), oss);
        if (!sdk)
            throw SW_RUNTIME_ERROR("No suitable installed WinSDK found for this host");
        ts["native"]["stdlib"]["c"] = sdk->getPackage().toString();
        //ts["os"]["version"] = sdkver->toString(3); // cut off the last (fourth) number

        auto clpkg = "com.Microsoft.VisualStudio.VC.cl";
        auto cl = getPredefinedTargets().find(clpkg);

        auto clangpppkg = "org.LLVM.clangpp";
        auto clangpp = getPredefinedTargets().find(clpkg);

        if (0);
#ifdef _MSC_VER
        // msvc + clangcl
        // clangcl must be compatible with msvc
        // and also clang actually
        else if (cl != getPredefinedTargets().end(clpkg) && !cl->second.empty())
        {
            ts["native"]["program"]["c"] = to_upkg("com.Microsoft.VisualStudio.VC.cl");
            ts["native"]["program"]["cpp"] = to_upkg("com.Microsoft.VisualStudio.VC.cl");
            ts["native"]["program"]["asm"] = to_upkg("com.Microsoft.VisualStudio.VC.ml");
            ts["native"]["program"]["lib"] = to_upkg("com.Microsoft.VisualStudio.VC.lib");
            ts["native"]["program"]["link"] = to_upkg("com.Microsoft.VisualStudio.VC.link");
        }
        // separate?
#else __clang__
        else if (clangpp != getPredefinedTargets().end(clangpppkg) && !clangpp->second.empty())
        {
            ts["native"]["program"]["c"] = to_upkg("org.LLVM.clang");
            ts["native"]["program"]["cpp"] = to_upkg("org.LLVM.clangpp");
            ts["native"]["program"]["asm"] = to_upkg("org.LLVM.clang");
            // ?
            ts["native"]["program"]["lib"] = to_upkg("com.Microsoft.VisualStudio.VC.lib");
            ts["native"]["program"]["link"] = to_upkg("com.Microsoft.VisualStudio.VC.link");
        }
#endif
        // add more defaults (clangcl, clang)
        else
            throw SW_RUNTIME_ERROR("Seems like you do not have Visual Studio installed.\nPlease, install the latest Visual Studio first.");
    }
    // add more defaults
    else
    {
        // set default libs?
        /*ts["native"]["stdlib"]["c"] = to_upkg("com.Microsoft.Windows.SDK.ucrt");
        ts["native"]["stdlib"]["cpp"] = to_upkg("com.Microsoft.VisualStudio.VC.libcpp");
        ts["native"]["stdlib"]["kernel"] = to_upkg("com.Microsoft.Windows.SDK.um");*/

        auto if_add = [this](auto &s, const UnresolvedPackage &name)
        {
            auto &pd = getPredefinedTargets();
            auto i = pd.find(name);
            if (i == pd.end() || i->second.empty())
                return false;
            s = name.toString();
            return true;
        };

        auto err_msg = [](const String &cl)
        {
            return "sw was built with " + cl + " as compiler, but it was not found in your system. Install " + cl + " to proceed.";
        };

        // must be the same compiler as current!
#if defined(__clang__)
        if (!(
            if_add(ts["native"]["program"]["c"], "org.LLVM.clang"s) &&
            if_add(ts["native"]["program"]["cpp"], "org.LLVM.clangpp"s)
            ))
        {
            throw SW_RUNTIME_ERROR(err_msg("clang"));
        }
        //if (getHostOs().is(OSType::Linux))
            //ts["native"]["stdlib"]["cpp"] = to_upkg("org.sw.demo.llvm_project.libcxx");
#elif defined(__GNUC__)
        if (!(
            if_add(ts["native"]["program"]["c"], "org.gnu.gcc") &&
            if_add(ts["native"]["program"]["cpp"], "org.gnu.gpp")
            ))
        {
            throw SW_RUNTIME_ERROR(err_msg("gcc"));
        }
#elif !defined(_WIN32)
#error "Add your current compiler to detect.cpp and here."
#endif

        // using c prog
        if_add(ts["native"]["program"]["asm"], ts["native"]["program"]["c"].getValue());

        // reconsider, also with driver?
        if_add(ts["native"]["program"]["lib"], "org.gnu.binutils.ar"s);

        // use driver
        // use cpp driver for the moment to not burden ourselves in adding stdlib
        if_add(ts["native"]["program"]["link"], ts["native"]["program"]["cpp"].getValue());
    }

    return ts;
}

void SwCoreContext::setHostSettings(const TargetSettings &s)
{
    host_settings = s;

    // always log!
    LOG_TRACE(logger, "New host configuration: " + getHostSettings().toString());
}

TargetData &SwCoreContext::getTargetData(const PackageId &pkg)
{
    return target_data[pkg];
}

const TargetData &SwCoreContext::getTargetData(const PackageId &pkg) const
{
    auto i = target_data.find(pkg);
    if (i == target_data.end())
        throw SW_RUNTIME_ERROR("No target data for package: " + pkg.toString());
    return i->second;
}

void SwCoreContext::setEntryPoint(const PackageId &pkgid, const TargetEntryPointPtr &ep)
{
    LocalPackage p(getLocalStorage(), pkgid);
    return setEntryPoint(p, ep);
}

void SwCoreContext::setEntryPoint(const LocalPackage &p, const TargetEntryPointPtr &ep)
{
    if (!ep)
        return;

    auto iep = entry_points.find(p);
    if (iep != entry_points.end())
    {
        if (iep->second != ep)
            throw SW_RUNTIME_ERROR("Setting entry point twice for package " + p.toString());
        //getTargetData(p); // "register" package
        return;
    }
    //getTargetData(p).setEntryPoint(ep);
    //getTargetData(p); // "register" package
    entry_points[p] = ep; // after target data

    // local package
    if (p.getPath().isRelative())
        return;

    //try
    //{
        setEntryPoint(p.getData().group_number, ep);
    //}
    //catch (...)
    //{
    //}
}

void SwCoreContext::setEntryPoint(PackageVersionGroupNumber gn, const TargetEntryPointPtr &ep)
{
    if (gn == 0)
        return;

    auto iepgn = entry_points_by_group_number.find(gn);
    if (iepgn != entry_points_by_group_number.end())
    {
        if (iepgn->second != ep)
            throw SW_RUNTIME_ERROR("Setting entry point twice for group_number " + std::to_string(gn));
        return;
    }
    entry_points_by_group_number[gn] = ep;
}

TargetEntryPointPtr SwCoreContext::getEntryPoint(const PackageId &pkgid) const
{
    LocalPackage p(getLocalStorage(), pkgid);
    return getEntryPoint(p);
}

TargetEntryPointPtr SwCoreContext::getEntryPoint(const LocalPackage &p) const
{
    auto gn = p.getData().group_number;
    if (gn == 0)
    {
        gn = get_specification_hash(read_file(p.getDirSrc2() / "sw.cpp"));
        p.setGroupNumber(gn);
        ((PackageData &)p.getData()).group_number = gn;
    }

    auto ep = getEntryPoint(gn);
    if (ep)
        return ep;
    auto i = entry_points.find(p);
    if (i != entry_points.end())
        return i->second;
    return {};
}

TargetEntryPointPtr SwCoreContext::getEntryPoint(PackageVersionGroupNumber p) const
{
    if (p == 0)
        //return {};
        throw SW_RUNTIME_ERROR("Empty entry point"); // assert?

    auto i = entry_points_by_group_number.find(p);
    if (i == entry_points_by_group_number.end())
        return {};
    return i->second;
}

SwContext::SwContext(const path &local_storage_root_dir)
    : SwCoreContext(local_storage_root_dir)
{
}

SwContext::~SwContext()
{
}

std::unique_ptr<SwBuild> SwContext::createBuild1()
{
    return std::make_unique<SwBuild>(*this, fs::current_path() / SW_BINARY_DIR);
}

std::unique_ptr<SwBuild> SwContext::createBuild()
{
    auto b = createBuild1();
    b->getTargets() = getPredefinedTargets();
    return std::move(b);
}

void SwContext::registerDriver(const PackageId &pkg, std::unique_ptr<IDriver> &&driver)
{
    auto [_, inserted] = drivers.insert_or_assign(pkg, std::move(driver));
    if (inserted)
        LOG_TRACE(logger, "Registering driver: " + pkg.toString());
}

void SwContext::executeBuild(const path &in)
{
    clearFileStorages();
    auto b = createBuild1();
    b->runSavedExecutionPlan(in);
}

Input &SwContext::addInput(const String &i)
{
    path p(i);
    if (fs::exists(p))
        return addInput(p);
    else
        return addInput(resolve(i));
}

Input &SwContext::addInput(const path &i)
{
    return addInput1(i);
}

Input &SwContext::addInput(const LocalPackage &p)
{
    auto &i = addInput1(p);
    if (i.isLoaded())
        return i;
    //if (getTargetData().find(p) == getTargetData().end())
        //return i;
    auto ep = getEntryPoint(p);
    if (ep)
        i.addEntryPoints({ ep });
    return i;
}

template <class I>
Input &SwContext::addInput1(const I &i)
{
    auto input = std::make_unique<Input>(i, *this);
    auto it = std::find_if(inputs.begin(), inputs.end(), [&i = *input](const auto &p)
    {
        return *p == i;
    });
    if (it != inputs.end())
        return **it;
    inputs.push_back(std::move(input));
    return *inputs.back();
}

void SwContext::loadEntryPoints(const std::set<Input*> &inputs, bool set_eps)
{
    std::map<IDriver *, std::vector<Input*>> active_drivers;
    for (auto &i : inputs)
    {
        if (!i->isLoaded())
            active_drivers[&i->getDriver()].push_back(i);
    }
    for (auto &[d, g] : active_drivers)
    {
        std::vector<RawInput> inputs;
        for (auto &i : g)
            inputs.push_back(*i);
        auto eps = d->createEntryPoints(*this, inputs);
        if (eps.size() != inputs.size())
            throw SW_RUNTIME_ERROR("Incorrect number of returned entry points");
        for (size_t i = 0; i < eps.size(); i++)
        {
            // when loading installed package, eps[i] may be empty
            // (ep already exists in driver)
            // so we take ep from context
            // test: sw build org.sw.demo.madler.zlib
            if (eps[i].empty())
            {
                if (inputs[i].getType() != InputType::InstalledPackage)
                    throw SW_RUNTIME_ERROR("unexpected input type");
                g[i]->addEntryPoints({ getEntryPoint(inputs[i].getPackageId()) });
            }
            else
                g[i]->addEntryPoints(eps[i]);

            if (!set_eps)
                continue;

            if (inputs[i].getType() != InputType::InstalledPackage)
            {
                // for non installed packages we must create entry points in sw context
                auto b = createBuild();
                auto s = getHostSettings();
                s["driver"]["dry-run"] = "true"; // used only to get pkgs list
                for (auto &ep : eps[i])
                {
                    auto tgts = ep->loadPackages(*b, s, {}, {});
                    for (auto &tgt : tgts)
                    {
                        PackageData d;
                        d.prefix = 0;
                        // add only gn atm
                        d.group_number = g[i]->getGroupNumber();
                        getLocalStorage().installLocalPackage(tgt->getPackage(), d);

                        setEntryPoint(LocalPackage(getLocalStorage(), tgt->getPackage()), ep);
                    }
                }
                continue;
            }

            /*for (auto &ep : eps[i])
            {
                // for packages we must also register all other group packages
                // which are located in this config AND which are deps of this input package id
                auto m = resolve(UnresolvedPackages{ inputs[i].getPackageId() });
                auto &p = m.find(inputs[i].getPackageId())->second;
                for (auto &d : p->getData().dependencies)
                {
                    auto &p2 = m.find(d)->second;
                    if (p2->getData().group_number != p->getData().group_number)
                        continue;
                    setEntryPoint(*p2, ep);
                }
            }*/
        }
    }
}

}
