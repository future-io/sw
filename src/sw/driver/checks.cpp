// Copyright (C) 2017-2018 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "checks.h"

#include "checks_storage.h"
#include "build.h"
#include "entry_point.h"
#include "target/native.h"

#include <sw/builder/execution_plan.h>
#include <sw/core/sw_context.h>
#include <sw/manager/storage.h>
#include <sw/support/filesystem.h>
#include <sw/support/hash.h>

#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>
#include <primitives/emitter.h>

#include <primitives/log.h>
DECLARE_STATIC_LOGGER(logger, "checks");

bool checks_single_thread;
bool print_checks;
bool wait_for_cc_checks;
String cc_checks_command;

namespace sw
{

static path getServiceDir(const path &bdir)
{
    return bdir / "misc";
}

static path getChecksDir(const path &bdir)
{
    return getServiceDir(bdir) / "checks";
}

static String toString(CheckType t)
{
    switch (t)
    {
    case CheckType::Function:
        return "function";
    case CheckType::Include:
        return "include";
    case CheckType::Type:
        return "type";
    case CheckType::TypeAlignment:
        return "alignment";
    case CheckType::Library:
        return "library";
    case CheckType::LibraryFunction:
        return "library function";
    case CheckType::Symbol:
        return "symbol";
    case CheckType::StructMember:
        return "struct member";
    case CheckType::SourceCompiles:
        return "source compiles";
    case CheckType::SourceLinks:
        return "source links";
    case CheckType::SourceRuns:
        return "source runs";
    case CheckType::Declaration:
        return "source declaration";
    case CheckType::Custom:
        return "custom";
    default:
        SW_UNREACHABLE;
    }
}

static std::unordered_map<String, std::unique_ptr<ChecksStorage>> &getChecksStorages()
{
    static std::unordered_map<String, std::unique_ptr<ChecksStorage>> checksStorages;
    return checksStorages;
}

static ChecksStorage &getChecksStorage(const String &config)
{
    auto i = getChecksStorages().find(config);
    if (i == getChecksStorages().end())
    {
        auto [i, _] = getChecksStorages().emplace(config, std::make_unique<ChecksStorage>());
        return *i->second;
    }
    return *i->second;
}

static ChecksStorage &getChecksStorage(const String &config, const path &fn)
{
    auto i = getChecksStorages().find(config);
    if (i == getChecksStorages().end())
    {
        auto [i, _] = getChecksStorages().emplace(config, std::make_unique<ChecksStorage>());
        i->second->load(fn);
        return *i->second;
    }
    return *i->second;
}

void ChecksStorage::load(const path &fn)
{
    if (loaded)
        return;

    {
        std::ifstream i(fn);
        if (!i)
            return;
        while (i)
        {
            size_t h;
            i >> h;
            if (!i)
                break;
            i >> all_checks[h];
        }
    }

    load_manual(fn);

    loaded = true;
}

void ChecksStorage::load_manual(const path &fn)
{
#define MANUAL_CHECKS ".manual.txt"
    auto mf = path(fn) += MANUAL_CHECKS;
    if (!fs::exists(mf))
        return;
    for (auto &l : read_lines(mf))
    {
        if (l[0] == '#')
            continue;
        auto v = split_string(l, " ");
        if (v.size() != 2)
            continue;
        //throw SW_RUNTIME_ERROR("bad manual checks line: " + l);
        if (v[1] == "?")
            continue;
        //throw SW_RUNTIME_ERROR("unset manual check: " + l);
        all_checks[std::stoull(v[0])] = std::stoi(v[1]);
        new_manual_checks_loaded = true;
    }
}

void ChecksStorage::save(const path &fn) const
{
    fs::create_directories(fn.parent_path());
    {
        String s;
        for (auto &[h, v] : std::map<decltype(all_checks)::key_type, decltype(all_checks)::mapped_type>(all_checks.begin(), all_checks.end()))
            s += std::to_string(h) + " " + std::to_string(v) + "\n";
        write_file(fn, s);
    }

    if (!manual_checks.empty())
    {
        String s;
        for (auto &[h, c] : std::map<decltype(manual_checks)::key_type, decltype(manual_checks)::mapped_type>(manual_checks.begin(), manual_checks.end()))
        {
            s += "# ";
            for (auto &d : c->Definitions)
                s += d + " ";
            s.resize(s.size() - 1);
            s += "\n";
            s += std::to_string(h) + " ?\n\n";
        }
        write_file(path(fn) += MANUAL_CHECKS, s);
    }
}

void ChecksStorage::add(const Check &c)
{
    auto h = c.getHash();
    if (c.requires_manual_setup && !c.Value)
    {
        manual_checks[h] = &c;
        return;
    }
    all_checks[h] = c.Value.value();
}

static String make_function_var(const String &d, const String &prefix = "HAVE_", const String &suffix = {})
{
    return prefix + boost::algorithm::to_upper_copy(d) + suffix;
}

static String make_include_var(const String &i)
{
    auto v_def = make_function_var(i);
    for (auto &c : v_def)
    {
        if (!isalnum(c))
            c = '_';
    }
    return v_def;
}

static String make_type_var(const String &t, const String &prefix = "HAVE_", const String &suffix = {})
{
    String v_def = make_function_var(t, prefix, suffix);
    for (auto &c : v_def)
    {
        if (c == '*')
            c = 'P';
        else if (!isalnum(c))
            c = '_';
    }
    return v_def;
}

static String make_struct_member_var(const String &s, const String &m)
{
    return make_include_var(s + " " + m);
}

static String make_alignment_var(const String &i)
{
    return make_type_var(i, "ALIGNOF_");
}

static void check_def(const String &d)
{
    if (d.empty())
        throw SW_RUNTIME_ERROR("Empty check definition");
}

CheckSet::CheckSet(Checker &checker)
    : checker(checker)
{
}

Checker::Checker(SwBuild &swbld)
    : swbld(swbld)
{
}

CheckSet &Checker::addSet(const String &name)
{
    auto cs = std::make_shared<CheckSet>(*this);
    auto p = sets.emplace(name, cs);
    p.first->second->name = name;
    return *p.first->second;
}

void CheckSet::performChecks(const TargetSettings &ts)
{
    static const auto checks_dir = checker.swbld.getContext().getLocalStorage().storage_dir_etc / "sw" / "checks";

    //std::unique_lock lk(m);

    auto config = ts.getHash();

    /*static std::mutex m;
    static std::map<String, std::mutex> checks_mutex;
    std::mutex *m2;
    {
        std::unique_lock lk(m);
        m2 = &checks_mutex[config];
    }
    std::unique_lock lk(*m2);
    //std::unique_lock lk2(m);*/

    auto fn = checks_dir / config / "checks.3.txt";
    auto &cs = getChecksStorage(config, fn);

    // add common checks
    checkSourceRuns("WORDS_BIGENDIAN", R"(
int IsBigEndian()
{
    volatile int i=1;
    return ! *((char *)&i);
}
int main() { return IsBigEndian(); }
)");

    // returns true if inserted
    auto add_dep = [this, &cs](auto &c)
    {
        auto h = c->getHash();
        auto ic = checks.find(h);
        if (ic != checks.end())
        {
            checks[h] = ic->second;
            ic->second->Definitions.insert(c->Definitions.begin(), c->Definitions.end());
            ic->second->Prefixes.insert(c->Prefixes.begin(), c->Prefixes.end());

            // maybe we already know it?
            // this path is used with wait_for_cc_checks
            auto i = cs.all_checks.find(h);
            if (i != cs.all_checks.end())
                ic->second->Value = i->second;

            return std::pair{ false, ic->second };
        }
        checks[h] = c;

        auto i = cs.all_checks.find(h);
        if (i != cs.all_checks.end())
            c->Value = i->second;
        return std::pair{ true, c };
    };

    // prepare loaded checks
    for (auto &c : all)
    {
        auto[inserted, dep] = add_dep(c);
        auto deps = c->gatherDependencies();
        for (auto &d : deps)
        {
            auto [inserted, dep2] = add_dep(d);
            dep->dependencies.insert(dep2);
        }

        // add to check_values only requested defs
        // otherwise we'll get also defs from other sets (e.g. with prefixes from ICU 'U_')
        for (auto &d : c->Definitions)
        {
            check_values[d];
            for (auto &p : c->Prefixes)
                check_values[p + d];
        }
    }

    // remove this?
    SCOPE_EXIT
    {
        this->all.clear();
    };

    // perform
    std::unordered_set<CheckPtr> unchecked;
    for (auto &[h, c] : checks)
    {
        if (!c->isChecked())
            unchecked.insert(c);
    }

    SCOPE_EXIT
    {
        prepareChecksForUse();
        if (print_checks)
        {
            std::ofstream o(fn.parent_path() / (t->getPackage().toString() + "." + name + ".txt"));
            if (!o)
                return;
            std::map<String, CheckPtr> cv(check_values.begin(), check_values.end());
            for (auto &[d, c] : cv)
            {
                if (c->Value)
                    o << d << " " << c->Value.value() << " " << c->getHash() << "\n";
            }
        }
        // cleanup
        for (auto &[h, c] : checks)
        {
            c->clean();
        }
    };

    if (print_checks)
    {
        write_file(checks_dir / config / "cfg.json", nlohmann::json::parse(ts.toString(TargetSettings::Json)).dump(4));
    }

    if (unchecked.empty())
    {
        if (cs.new_manual_checks_loaded)
            cs.save(fn);
        return;
    }

    auto ep = ExecutionPlan::create(unchecked);
    if (ep)
    {
        LOG_INFO(logger, "Performing " << unchecked.size() << " check(s): "
            << t->getPackage().toString() << " (" << name << "), config " + config);

        SCOPE_EXIT
        {
            // remove tmp dir
            error_code ec;
            fs::remove_all(getChecksDir(checker.swbld.getBuildDirectory()), ec);
        };

        //auto &e = getExecutor();
        static Executor e(checks_single_thread ? 1 : getExecutor().numberOfThreads()); // separate executor!

        try
        {
            ep.execute(e);
        }
        catch (...)
        {
            // in case of error, some checks may be unchecked
            // and we record only checked checks
            for (auto &[h, c] : checks)
            {
                if (c->Value)
                    cs.add(*c);
            }
            cs.save(fn);
            throw;
        }

        for (auto &[h, c] : checks)
            cs.add(*c);

        auto cc_dir = fn.parent_path() / "cc";

        // separate loop
        if (!cs.manual_checks.empty())
        {
            std::error_code ec;
            fs::remove_all(cc_dir, ec);
            if (ec)
                LOG_WARN(logger, "Cannot remove checks dir: " + cc_dir.u8string());
            fs::create_directories(cc_dir, ec);

            for (auto &[h, c] : checks)
            {
                if (c->requires_manual_setup)
                {
                    auto dst = (cc_dir / std::to_string(c->getHash())) += BuildSettings(ts).TargetOS.getExecutableExtension();
                    if (!fs::exists(dst))
                        fs::copy_file(c->executable, dst, fs::copy_options::overwrite_existing);
                }
            }
        }

        // save
        cs.save(fn);

        if (!cs.manual_checks.empty())
        {
            // prevent multiple threads, but allow us to enter more than one time
            static std::recursive_mutex m;
            std::unique_lock lk(m);

            // save executables
            auto os = BuildSettings(ts).TargetOS;
            auto mfn = (path(fn) += MANUAL_CHECKS).filename().u8string();

            auto bat = os.getShellType() == ShellType::Batch;

            primitives::Emitter ctx;
            if (!bat)
            {
                ctx.addLine("#!/bin/sh");
                ctx.addLine();
            }

            ctx.addLine("OUTF=\"" + mfn + "\"");
            ctx.addLine("OUT=\""s + (wait_for_cc_checks ? "../" : "") + "$OUTF\"");
            ctx.addLine();

            mfn = "$OUT";
            ctx.addLine("echo \"\" > " + mfn);
            ctx.addLine();

            for (auto &[h, c] : cs.manual_checks)
            {
                String defs;
                for (auto &d : c->Definitions)
                    defs += d + " ";
                defs.resize(defs.size() - 1);

                ctx.addLine((bat ? "::"s : "#"s) + " " + defs);
                //if (!bat)
                //s += "-n ";

                auto fn = std::to_string(c->getHash());

                ctx.increaseIndent("if [ ! -f " + fn + " ]; then");
                ctx.addLine("echo missing file: " + fn);
                ctx.addLine("exit 1");
                ctx.decreaseIndent("fi");

                ctx.addLine("echo \"Checking: " + defs + "... \"");
                ctx.addLine("echo \"# " + defs + "\" >> " + mfn);

                if (!bat)
                {
                    ctx.addLine("chmod 755 " + fn);
                    ctx.addLine("./");
                }
                ctx.addText(fn + BuildSettings(ts).TargetOS.getExecutableExtension());

                ctx.addLine("echo " + std::to_string(c->getHash()) + " ");
                if (!bat)
                    ctx.addText("$? ");
                else
                    ctx.addText("%errorlevel% ");
                ctx.addText(">> " + mfn);
                if (!bat)
                    ctx.addLine("echo ok");
                ctx.addLine("echo \"\" >> " + mfn);
                ctx.addLine();

            }
            path out = (cc_dir / "run") += os.getShellExtension();
            write_file(out, ctx.getText());

            if (wait_for_cc_checks)
            {
                if (!cc_checks_command.empty())
                {
                    ScopedCurrentPath scp(cc_dir);
                    int r = system(cc_checks_command.c_str());
                    if (r)
                        throw SW_RUNTIME_ERROR("cc_checks_command exited abnormally: " + std::to_string(r));
                }
                else
                {
                    std::cout << "Waiting for completing cc checks.\n";
                    std::cout << "Run '" << normalize_path(out) << "' and press and key to continue...\n";
                    getchar();
                }
                cs.load_manual(fn);
                for (auto &[h, c] : cs.manual_checks)
                {
                    if (cs.all_checks.find(h) == cs.all_checks.end())
                        continue;
                    c->requires_manual_setup = false;
                }
                cs.manual_checks.clear();
                return performChecks(ts);
            }

            throw SW_RUNTIME_ERROR("Some manual checks are missing, please set them in order to continue. "
                "Manual checks file: " + (path(fn) += MANUAL_CHECKS).u8string() + ". "
                "You also may copy produced binaries to target platform and run them there using prepared script. "
                "Results will be gathered into required file. "
                "Binaries directory: " + cc_dir.u8string()
            );
        }

        return;
    }

    // error!

    // print our deps graph
    String s;
    s += "digraph G {\n";
    for (auto &c : ep.getUnprocessedCommandSet())
    {
        for (auto &d : c->dependencies)
        {
            if (ep.getUnprocessedCommandSet().find(static_cast<Check*>(d.get())) == ep.getUnprocessedCommandSet().end())
                continue;
            s += *static_cast<Check*>(c)->Definitions.begin() + "->" + *std::static_pointer_cast<Check>(d)->Definitions.begin() + ";";
        }
    }
    s += "}";

    auto d = getServiceDir(checker.swbld.getBuildDirectory());
    auto cyclic_path = d / "cyclic";
    write_file(cyclic_path / "deps_checks.dot", s);

    throw SW_RUNTIME_ERROR("Cannot create execution plan because of cyclic dependencies");
}

Check::~Check()
{
    clean();
}

void Check::clean() const
{
    commands.clear();
}

String Check::getName(bool short_name) const
{
    auto d = getDefinition();
    if (d)
        return *d;
    return {};
}

std::optional<String> Check::getDefinition() const
{
    return getDefinition(*Definitions.begin());
}

std::optional<String> Check::getDefinition(const String &d) const
{
    if (Value.value() != 0 || DefineIfZero)
        return d + "=" + std::to_string(Value.value());
    return std::nullopt;
}

bool Check::isChecked() const
{
    return !!Value;
}

size_t CheckParameters::getHash() const
{
    size_t h = 0;
    hash_combine(h, cpp);
    for (auto &d : Definitions)
        hash_combine(h, d);
    for (auto &d : Includes)
        hash_combine(h, d);
    for (auto &d : IncludeDirectories)
        hash_combine(h, d);
    for (auto &d : Libraries)
        hash_combine(h, d);
    for (auto &d : Options)
        hash_combine(h, d);
    return h;
}

size_t Check::getHash() const
{
    size_t h = 0;
    hash_combine(h, data);
    hash_combine(h, Parameters.getHash());
    hash_combine(h, CPP);
    return h;
}

void Check::execute()
{
    if (isChecked())
        return;
    //Value = 0; // mark as checked

    String log_string = "[" + std::to_string((*current_command)++) + "/" + std::to_string(total_commands->load()) + "] ";
    //LOG_TRACE(logger, "Checking " << data);

    // value must be set inside?
    run();

    if (Definitions.empty())
        throw SW_RUNTIME_ERROR(log_string + "Check " + data + ": definition was not set");
    if (!Value)
    {
        if (requires_manual_setup)
        {
            LOG_INFO(logger, log_string + "Check " << *Definitions.begin() << " requires to be set up manually");
            return;
        }
        throw SW_RUNTIME_ERROR(log_string + "Check " + *Definitions.begin() + ": value was not set");
    }
    LOG_DEBUG(logger, log_string + "Checking " << toString(getType()) << " " << *Definitions.begin() << ": " << Value.value());
}

std::vector<CheckPtr> Check::gatherDependencies()
{
    std::vector<CheckPtr> deps;
    for (auto &d : Parameters.Includes)
        deps.push_back(check_set->add<IncludeExists>(d));
    return deps;
}

bool Check::lessDuringExecution(const CommandNode &in) const
{
    // improve sorting! it's too stupid
    // simple "0 0 0 0 1 2 3 6 7 8 9 11" is not enough

    auto &rhs = (const Check &)in;

    if (dependencies.size() != rhs.dependencies.size())
        return dependencies.size() < rhs.dependencies.size();
    return dependent_commands.size() > dependent_commands.size();
}

const path &Check::getUniqueName() const
{
    if (uniq_name.empty())
        uniq_name = unique_path();
    return uniq_name;
}

path Check::getOutputFilename() const
{
    auto d = getChecksDir(check_set->checker.swbld.getBuildDirectory());
    auto up = getUniqueName();
    d /= up;
    auto f = d;
    if (!CPP)
        f /= "x.c";
    else
        f /= "x.cpp";
    return f;
}

static path getUniquePath(const path &p)
{
    return boost::replace_all_copy(p.parent_path().filename().u8string(), "-", "_");
}

static String getTargetName(const path &p)
{
    return "loc." + getUniquePath(p).string();
}

Build Check::setupSolution(SwBuild &b, const path &f) const
{
    Build s(b);
    s.BinaryDir = f.parent_path();
    s.NamePrefix.clear();
    s.DryRun = false;
    return s;
}

TargetSettings Check::getSettings() const
{
    auto ss = check_set->t->getSettings();

    // some checks may fail in msvc release (functions become intrinsics (mem*) etc.)
    if (check_set->t->getCompilerType() == CompilerType::MSVC ||
        check_set->t->getCompilerType() == CompilerType::ClangCl)
        ss["native"]["configuration"] = "debug";

    // set output dir for check binaries
    auto d = getChecksDir(check_set->checker.swbld.getBuildDirectory());
    auto up = getUniqueName();
    d /= up;
    ss["output_dir"] = normalize_path(d);
    ss["output_dir"].useInHash(false);
    ss["output_dir"].ignoreInComparison(true);

    return ss;
}

void Check::setupTarget(NativeCompiledTarget &t) const
{
    t.GenerateWindowsResource = false;
    if (auto L = t.getSelectedTool()->as<VisualStudioLinker*>())
        L->DisableIncrementalLink = true;
    t.command_storage = nullptr;
}

bool Check::execute(SwBuild &b) const
{
    b.overrideBuildState(BuildState::InputsLoaded);
    b.setTargetsToBuild();
    b.resolvePackages();
    b.loadPackages();
    b.prepare();

    try
    {
        // save commands for cleanup
        auto p = b.getExecutionPlan();
        // we must save comands here, because later we need to check results of specific commands
        for (auto &c : p.getCommands())
            commands.push_back(std::static_pointer_cast<builder::Command>(c->shared_from_this()));
        p.silent = true;

        b.execute(p);
    }
    catch (std::exception &e)
    {
        Value = 0;
        LOG_TRACE(logger, "Check " + data + ": check issue: " << e.what());
        return false;
    }
    return true;
}

#define SETUP_SOLUTION()                                          \
    auto b = check_set->checker.swbld.getContext().createBuild(); \
    auto s = setupSolution(*b, f);                                \
    s.module_data.current_settings = getSettings()

#define EXECUTE_SOLUTION()                             \
    for (auto &t : s.module_data.added_targets)        \
        b->getTargets()[t->getPackage()].push_back(t); \
    if (!execute(*b))                                  \
    return

FunctionExists::FunctionExists(const String &f, const String &def)
{
    if (f.empty())
        throw SW_RUNTIME_ERROR("Empty function");
    data = f;

    if (def.empty())
        Definitions.insert(make_function_var(data));
    else
        Definitions.insert(def);

    check_def(*Definitions.begin());
}

String FunctionExists::getSourceFileContents() const
{
    static const String src{ R"(
#ifdef __cplusplus
extern "C"
#endif
  char
  CHECK_FUNCTION_EXISTS(void);
#ifdef __CLASSIC_C__
int main()
{
  int ac;
  char* av[];
#else
int main(int ac, char* av[])
{
#endif
  CHECK_FUNCTION_EXISTS();
  if (ac > 1000) {
    return *av[0];
  }
  return 0;
}
)"
    };

    return src;
}

void FunctionExists::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    e.Definitions["CHECK_FUNCTION_EXISTS"] = data; // before setup, because it is changed later for LibraryFunctionExists
    setupTarget(e);
    e += f;

    EXECUTE_SOLUTION();

    auto cmd = e.getCommand();
    Value = (cmd && cmd->exit_code && cmd->exit_code.value() == 0) ? 1 : 0;
}

IncludeExists::IncludeExists(const String &i, const String &def)
{
    if (i.empty())
        throw SW_RUNTIME_ERROR("Empty include");
    data = i;

    if (def.empty())
    {
        Definitions.insert(make_include_var(data));

        // some libs expect HAVE_SYSTIME_H and not HAVE_SYS_TIME_H
        if (data.find("sys/") == 0)
        {
            auto d2 = data;
            d2 = "sys" + data.substr(4);
            Definitions.insert(make_include_var(d2));
        }
    }
    else
        Definitions.insert(def);

    check_def(*Definitions.begin());
}

String IncludeExists::getSourceFileContents() const
{
    String src = "#include <" + data + ">";
    if (!CPP)
        src += R"(
#ifdef __CLASSIC_C__
int main()
{
  return 0;
}
#else
int main(void)
{
  return 0;
}
#endif
)";
    else
        src += R"(
int main()
{
  return 0;
}
)";

    return src;
}

void IncludeExists::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    setupTarget(e);
    e += f;

    EXECUTE_SOLUTION();

    auto cmd = e.getCommand();
    Value = (cmd && cmd->exit_code && cmd->exit_code.value() == 0) ? 1 : 0;
}

TypeSize::TypeSize(const String &t, const String &def)
{
    if (t.empty())
        throw SW_RUNTIME_ERROR("Empty type");
    data = t;

    Definitions.insert(make_type_var(data));
    Definitions.insert(make_type_var(data, "SIZEOF_"));
    // some cmake new thing
    // https://cmake.org/cmake/help/latest/module/CheckTypeSize.html
    Definitions.insert(make_type_var(data, "SIZEOF_", "_CODE"));
    Definitions.insert(make_type_var(data, "SIZE_OF_"));
    // some libs want these
    Definitions.insert(make_type_var(data, "HAVE_SIZEOF_"));
    Definitions.insert(make_type_var(data, "HAVE_SIZE_OF_"));

    if (!def.empty())
        Definitions.insert(def);

    check_def(*Definitions.begin());

    for (auto &h : { "sys/types.h", "stdint.h", "stddef.h", "inttypes.h" })
        Parameters.Includes.push_back(h);
}

String TypeSize::getSourceFileContents() const
{
    String src;
    for (auto &d : Parameters.Includes)
    {
        auto c = check_set->get<IncludeExists>(d);
        if (c->Value && c->Value.value())
            src += "#include <" + d + ">\n";
    }
    src += "int main() { return sizeof(" + data + "); }";

    return src;
}

void TypeSize::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    setupTarget(e);
    e += f;

    EXECUTE_SOLUTION();

    auto cmd = e.getCommand();
    if (!cmd)
    {
        Value = 0;
        return;
    }

    if (!check_set->t->getContext().getHostOs().canRunTargetExecutables(check_set->t->getBuildSettings().TargetOS))
    {
        requires_manual_setup = true;
        executable = e.getOutputFile();
        return;
    }

    primitives::Command c;
    c.setProgram(e.getOutputFile());
    error_code ec;
    c.execute(ec);
    Value = c.exit_code;
}

TypeAlignment::TypeAlignment(const String &t, const String &def)
{
    if (t.empty())
        throw SW_RUNTIME_ERROR("Empty type");
    data = t;

    if (def.empty())
        Definitions.insert(make_alignment_var(data));
    else
        Definitions.insert(def);

    check_def(*Definitions.begin());

    for (auto &h : { "sys/types.h", "stdint.h", "stddef.h", "stdio.h", "stdlib.h", "inttypes.h" })
        Parameters.Includes.push_back(h);
}

String TypeAlignment::getSourceFileContents() const
{
    String src;
    for (auto &d : Parameters.Includes)
    {
        auto c = check_set->get<IncludeExists>(d);
        if (c->Value && c->Value.value())
            src += "#include <" + d + ">\n";
    }
    src += R"(
int main()
{
    char diff;
    struct foo {char a; )" + data + R"( b;};
    struct foo *p = (struct foo *) malloc(sizeof(struct foo));
    diff = ((char *)&p->b) - ((char *)&p->a);
    return diff;
}
)";

    return src;
}

void TypeAlignment::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    setupTarget(e);
    e += f;

    EXECUTE_SOLUTION();

    auto cmd = e.getCommand();
    if (!cmd)
    {
        Value = 0;
        return;
    }

    if (!check_set->t->getContext().getHostOs().canRunTargetExecutables(check_set->t->getBuildSettings().TargetOS))
    {
        requires_manual_setup = true;
        executable = e.getOutputFile();
        return;
    }

    primitives::Command c;
    c.setProgram(e.getOutputFile());
    error_code ec;
    c.execute(ec);
    Value = c.exit_code;
}

SymbolExists::SymbolExists(const String &s, const String &def)
{
    if (s.empty())
        throw SW_RUNTIME_ERROR("Empty symbol");
    data = s;

    if (def.empty())
        Definitions.insert(make_function_var(data));
    else
        Definitions.insert(def);

    check_def(*Definitions.begin());
}

String SymbolExists::getSourceFileContents() const
{
    String src;
    for (auto &d : Parameters.Includes)
    {
        auto c = check_set->get<IncludeExists>(d);
        if (c->Value && c->Value.value())
            src += "#include <" + d + ">\n";
    }
    src += R"(
int main(int argc, char** argv)
{
  (void)argv;
#ifndef )" + data + R"(
  return ((int*)(&)" + data + R"())[argc];
#else
  (void)argc;
  return 0;
#endif
}
)";

    return src;
}

void SymbolExists::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    setupTarget(e);
    e += f;

    EXECUTE_SOLUTION();

    Value = 1;
}

DeclarationExists::DeclarationExists(const String &d, const String &def)
{
    if (d.empty())
        throw SW_RUNTIME_ERROR("Empty declaration");
    data = d;

    if (def.empty())
        Definitions.insert(make_function_var(data, "HAVE_DECL_"));
    else
        Definitions.insert(def);

    check_def(*Definitions.begin());

    for (auto &h : { "sys/types.h",
                    "stdint.h",
                    "stddef.h",
                    "inttypes.h",
                    "stdio.h",
                    "sys/stat.h",
                    "stdlib.h",
                    "memory.h",
                    "string.h",
                    "strings.h",
                    "unistd.h" })
        Parameters.Includes.push_back(h);
}

String DeclarationExists::getSourceFileContents() const
{
    String src;
    for (auto &d : Parameters.Includes)
    {
        auto c = check_set->get<IncludeExists>(d);
        if (c->Value && c->Value.value())
            src += "#include <" + d + ">\n";
    }
    src += "int main() { (void)" + data + "; return 0; }";

    return src;
}

void DeclarationExists::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    setupTarget(e);
    e += f;

    EXECUTE_SOLUTION();

    auto cmd = e.getCommand();
    Value = (cmd && cmd->exit_code && cmd->exit_code.value() == 0) ? 1 : 0;
}

StructMemberExists::StructMemberExists(const String &struct_, const String &member, const String &def)
    : struct_(struct_), member(member)
{
    if (struct_.empty() || member.empty())
        throw SW_RUNTIME_ERROR("Empty struct/member");
    data = struct_ + "." + member;

    if (def.empty())
        Definitions.insert(make_struct_member_var(struct_, member));
    else
        Definitions.insert(def);

    check_def(*Definitions.begin());
}

size_t StructMemberExists::getHash() const
{
    auto h = Check::getHash();
    hash_combine(h, struct_);
    hash_combine(h, member);
    return h;
}

String StructMemberExists::getSourceFileContents() const
{
    String src;
    for (auto &d : Parameters.Includes)
    {
        auto c = check_set->get<IncludeExists>(d);
        if (c->Value && c->Value.value())
            src += "#include <" + d + ">\n";
    }
    src += "int main() { sizeof(((" + struct_ + " *)0)->" + member + "); return 0; }";

    return src;
}

void StructMemberExists::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    setupTarget(e);
    e += f;

    EXECUTE_SOLUTION();

    auto cmd = e.getCommand();
    Value = (cmd && cmd->exit_code && cmd->exit_code.value() == 0) ? 1 : 0;
}

LibraryFunctionExists::LibraryFunctionExists(const String &library, const String &function, const String &def)
    : library(library), function(function)
{
    if (library.empty() || function.empty())
        throw SW_RUNTIME_ERROR("Empty library/function");
    data = library + "." + function;

    if (def.empty())
        Definitions.insert(make_function_var(function));
    else
        Definitions.insert(def);

    check_def(*Definitions.begin());
}

size_t LibraryFunctionExists::getHash() const
{
    auto h = Check::getHash();
    hash_combine(h, library);
    hash_combine(h, function);
    return h;
}

void LibraryFunctionExists::setupTarget(NativeCompiledTarget &e) const
{
    FunctionExists::setupTarget(e);
    e.Definitions["CHECK_FUNCTION_EXISTS"] = function;
    e.NativeLinkerOptions::System.LinkLibraries.push_back(library);
}

SourceCompiles::SourceCompiles(const String &def, const String &source)
{
    if (def.empty() || source.empty())
        throw SW_RUNTIME_ERROR("Empty def/source");
    data = source;
    Definitions.insert(def);
    check_def(*Definitions.begin());
}

String SourceCompiles::getSourceFileContents() const
{
    return data;
}

void SourceCompiles::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    setupTarget(e);
    for (auto &f : compiler_flags)
        e.CompileOptions.push_back(f);
    e += f;

    EXECUTE_SOLUTION();

    auto cmds = e.getCommands();
    cmds.erase(e.getCommand());
    if (cmds.size() != 1)
        return;
    auto &cmd = *cmds.begin();
    Value = (cmd && cmd->exit_code && cmd->exit_code.value() == 0) ? 1 : 0;
}

SourceLinks::SourceLinks(const String &def, const String &source)
{
    if (def.empty() || source.empty())
        throw SW_RUNTIME_ERROR("Empty def/source");
    data = source;
    Definitions.insert(def);
    check_def(*Definitions.begin());
}

String SourceLinks::getSourceFileContents() const
{
    return data;
}

void SourceLinks::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    setupTarget(e);
    e += f;

    EXECUTE_SOLUTION();

    Value = 1;
}

SourceRuns::SourceRuns(const String &def, const String &source)
{
    if (def.empty() || source.empty())
        throw SW_RUNTIME_ERROR("Empty def/source");
    data = source;
    Definitions.insert(def);
    check_def(*Definitions.begin());
}

String SourceRuns::getSourceFileContents() const
{
    return data;
}

void SourceRuns::run() const
{
    auto f = getOutputFilename();
    write_file(f, getSourceFileContents());

    SETUP_SOLUTION();

    auto &e = s.addTarget<ExecutableTarget>(getTargetName(f));
    setupTarget(e);
    e += f;

    EXECUTE_SOLUTION();

    auto cmd = e.getCommand();
    if (!cmd)
    {
        Value = 0;
        return;
    }

    if (!check_set->t->getContext().getHostOs().canRunTargetExecutables(check_set->t->getBuildSettings().TargetOS))
    {
        requires_manual_setup = true;
        executable = e.getOutputFile();
        return;
    }

    primitives::Command c;
    c.setProgram(e.getOutputFile());
    error_code ec;
    c.execute(ec);
    Value = c.exit_code;
}

CompilerFlag::CompilerFlag(const String &def, const String &compiler_flag)
    : SourceCompiles(def, "int main() {return 0;}")
{
    compiler_flags.push_back(compiler_flag);
}

CompilerFlag::CompilerFlag(const String &def, const Strings &compiler_flags)
    : SourceCompiles(def, "int main() {return 0;}")
{
    for (auto &f : compiler_flags)
        this->compiler_flags.push_back(f);
}

FunctionExists &CheckSet1::checkFunctionExists(const String &function, bool cpp)
{
    auto c = add<FunctionExists>(function);
    c->CPP = cpp;
    return *c;
}

FunctionExists &CheckSet1::checkFunctionExists(const String &function, const String &def, bool cpp)
{
    auto c = add<FunctionExists>(function, def);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkIncludeExists(const String &include, bool cpp)
{
    auto c = add<IncludeExists>(include);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkIncludeExists(const String &include, const String &def, bool cpp)
{
    auto c = add<IncludeExists>(include, def);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkLibraryFunctionExists(const String &library, const String &function, bool cpp)
{
    auto c = add<LibraryFunctionExists>(library, function);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkLibraryFunctionExists(const String &library, const String &function, const String &def, bool cpp)
{
    auto c = add<LibraryFunctionExists>(library, function, def);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkLibraryExists(const String &library, bool cpp)
{
    return *add<FunctionExists>(library);
}

Check &CheckSet1::checkLibraryExists(const String &library, const String &def, bool cpp)
{
    return *add<FunctionExists>(library);
}

Check &CheckSet1::checkSymbolExists(const String &symbol, bool cpp)
{
    auto c = add<SymbolExists>(symbol);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkSymbolExists(const String &symbol, const String &def, bool cpp)
{
    auto c = add<SymbolExists>(symbol, def);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkStructMemberExists(const String &s, const String &member, bool cpp)
{
    auto c = add<StructMemberExists>(s, member);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkStructMemberExists(const String &s, const String &member, const String &def, bool cpp)
{
    auto c = add<StructMemberExists>(s, member, def);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkDeclarationExists(const String &decl, bool cpp)
{
    auto c = add<DeclarationExists>(decl);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkDeclarationExists(const String &decl, const String &def, bool cpp)
{
    auto c = add<DeclarationExists>(decl, def);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkTypeSize(const String &type, bool cpp)
{
    auto c = add<TypeSize>(type);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkTypeSize(const String &type, const String &def, bool cpp)
{
    auto c = add<TypeSize>(type, def);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkTypeAlignment(const String &type, bool cpp)
{
    auto c = add<TypeAlignment>(type);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkTypeAlignment(const String &type, const String &def, bool cpp)
{
    auto c = add<TypeAlignment>(type, def);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkSourceCompiles(const String &def, const String &src, bool cpp)
{
    auto c = add<SourceCompiles>(def, src);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkSourceLinks(const String &def, const String &src, bool cpp)
{
    auto c = add<SourceLinks>(def, src);
    c->CPP = cpp;
    return *c;
}

Check &CheckSet1::checkSourceRuns(const String &def, const String &src, bool cpp)
{
    auto c = add<SourceRuns>(def, src);
    c->CPP = cpp;
    return *c;
}

void CheckSet::prepareChecksForUse()
{
    for (auto &[h, c] : checks)
    {
        for (auto &d : c->Definitions)
        {
            if (check_values.find(d) != check_values.end())
                check_values[d] = c;
            for (auto &p : c->Prefixes)
            {
                if (check_values.find(p + d) != check_values.end())
                    check_values[p + d] = c;
            }
        }
    }
}

}
