// Copyright (C) 2017-2018 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.h"

#include <primitives/command.h>
#include <primitives/executor.h>

#include <condition_variable>
#include <mutex>

#define SW_INTERNAL_ADD_COMMAND(name, target) \
    (target).Storage.push_back(name)

#define SW_MAKE_CUSTOM_COMMAND(type, name, target, ...) \
    auto name = std::make_shared<type>((target).getMainBuild().getContext(), __VA_ARGS__)

#ifdef _MSC_VER
#define SW_MAKE_CUSTOM_COMMAND_AND_ADD(type, name, target, ...) \
    SW_MAKE_CUSTOM_COMMAND(type, name, target, __VA_ARGS__);    \
    SW_INTERNAL_ADD_COMMAND(name, target)
#else
#define SW_MAKE_CUSTOM_COMMAND_AND_ADD(type, name, target, ...) \
    SW_MAKE_CUSTOM_COMMAND(type, name, target, ##__VA_ARGS__);  \
    SW_INTERNAL_ADD_COMMAND(name, target)
#endif

/*
#define SW_MAKE_COMMAND(name, target) \
    SW_MAKE_CUSTOM_COMMAND(Command, name, target)
*/
#define SW_MAKE_COMMAND_AND_ADD(name, target) \
    SW_MAKE_CUSTOM_COMMAND_AND_ADD(Command, name, target)

/*
#define _SW_MAKE_EXECUTE_COMMAND(name, target) \
    SW_MAKE_CUSTOM_COMMAND(ExecuteCommand, name, target, __FILE__, __LINE__)
*/
#define _SW_MAKE_EXECUTE_COMMAND_AND_ADD(name, target) \
    SW_MAKE_CUSTOM_COMMAND_AND_ADD(ExecuteCommand, name, target, __FILE__, __LINE__)

// ExecuteBuiltinCommand
#ifdef _MSC_VER
/*
#define SW_MAKE_EXECUTE_BUILTIN_COMMAND(var_name, target, func_name, ...) \
    SW_MAKE_CUSTOM_COMMAND(::sw::builder::ExecuteBuiltinCommand, var_name, target, func_name, __VA_ARGS__)
*/
#define SW_MAKE_EXECUTE_BUILTIN_COMMAND_AND_ADD(var_name, target, func_name, ...) \
    SW_MAKE_CUSTOM_COMMAND_AND_ADD(::sw::builder::ExecuteBuiltinCommand, var_name, target, func_name, __VA_ARGS__)
#else
/*
#define SW_MAKE_EXECUTE_BUILTIN_COMMAND(var_name, target, func_name, ...) \
    SW_MAKE_CUSTOM_COMMAND(::sw::builder::ExecuteBuiltinCommand, var_name, target, func_name, ## __VA_ARGS__)
*/
#define SW_MAKE_EXECUTE_BUILTIN_COMMAND_AND_ADD(var_name, target, func_name, ...) \
    SW_MAKE_CUSTOM_COMMAND_AND_ADD(::sw::builder::ExecuteBuiltinCommand, var_name, target, func_name, ## __VA_ARGS__)
#endif

namespace sw
{

struct FileStorage;
struct Program;
struct SwBuilderContext;
struct CommandStorage;

struct SW_BUILDER_API CommandNode : std::enable_shared_from_this<CommandNode>
{
    using SPtr = std::shared_ptr<CommandNode>;

    std::unordered_set<SPtr> dependencies;

    std::atomic_size_t dependencies_left = 0;
    std::unordered_set<SPtr> dependent_commands;

    std::atomic_size_t *current_command = nullptr;
    std::atomic_size_t *total_commands = nullptr;

    CommandNode();
    CommandNode(const CommandNode &);
    CommandNode &operator=(const CommandNode &);
    virtual ~CommandNode();

    virtual String getName(bool short_name = false) const = 0;
    virtual void execute() = 0;
    virtual void prepare() = 0;
    virtual bool lessDuringExecution(const CommandNode &) const = 0;

    void clear()
    {
        dependent_commands.clear();
        dependencies.clear();
    }
};

struct SW_BUILDER_API ResourcePool
{
    int n = -1; // unlimited
    std::condition_variable cv;
    std::mutex m;

    void lock()
    {
        if (n == -1)
            return;
        std::unique_lock lk(m);
        cv.wait(lk, [this] { return n > 0; });
        --n;
    }

    void unlock()
    {
        if (n == -1)
            return;
        std::unique_lock lk(m);
        ++n;
        lk.unlock();
        cv.notify_one();
    }
};

namespace builder
{

using ::primitives::command::QuoteType;

namespace detail
{

#pragma warning(push)
#pragma warning(disable:4275) // warning C4275: non dll-interface struct 'primitives::Command' used as base for dll-interface struct 'sw::builder::Command'

struct SW_BUILDER_API ResolvableCommand : ::primitives::Command
{
#pragma warning(pop)

    using ::primitives::Command::push_back;
    path resolveProgram(const path &p) const override;
};

}

struct SW_BUILDER_API Command : ICastable, CommandNode, detail::ResolvableCommand // hide?
{
    using Base = detail::ResolvableCommand;
    using Clock = std::chrono::high_resolution_clock;

    String name;
    String name_short;

    Files inputs;
    // byproducts
    // used only to clean files and pre-create dirs
    //Files intermediate;
    // if some commands accept pairs of args, and specific outputs depend on specific inputs
    // C I1 O1 I2 O2
    // then split that command!
    Files outputs;
    Files implicit_inputs;

    // additional create dirs
    Files output_dirs;

    fs::file_time_type mtime = fs::file_time_type::min();
    std::optional<bool> use_response_files;
    int first_response_file_argument = 0;
    bool remove_outputs_before_execution = false; // was true
    bool protect_args_with_quotes = true;
    bool always = false;
    bool do_not_save_command = false;
    bool silent = false; // no log record
    bool show_output = false; // no command output
    bool write_output_to_file = false;
    int strict_order = 0; // used to execute this before other commands
    ResourcePool *pool = nullptr;

    std::thread::id tid;
    Clock::time_point t_begin;
    Clock::time_point t_end;

    enum
    {
        MU_FALSE    = 0,
        MU_TRUE     = 1,
        MU_ALWAYS   = 2,
    };
    int maybe_unused = 0;

    path command_storage_root; // used during deserialization to restore command_storage
    CommandStorage *command_storage = nullptr;

    Command() = default;
    Command(const SwBuilderContext &swctx);
    virtual ~Command();

    void prepare() override;
    void execute() override;
    void execute(std::error_code &ec) override;
    void clean() const;
    bool isExecuted() const { return pid != -1 || executed_; }

    String getName(bool short_name = false) const override;

    virtual bool isOutdated() const;
    bool needsResponseFile() const;
    bool needsResponseFile(size_t sz) const;

    using Base::push_back;
    using Base::setProgram;
    void setProgram(std::shared_ptr<Program> p);
    void addInput(const path &p);
    void addInput(const Files &p);
    void addImplicitInput(const path &p);
    void addImplicitInput(const Files &p);
    void addOutput(const path &p);
    void addOutput(const Files &p);
    path redirectStdin(const path &p);
    path redirectStdout(const path &p, bool append = false);
    path redirectStderr(const path &p, bool append = false);
    size_t getHash() const;
    Files getGeneratedDirs() const; // used by generators
    void addInputOutputDeps();
    path writeCommand(const path &basename, bool print_name = true) const;

    bool lessDuringExecution(const CommandNode &rhs) const override;

    void onBeforeRun() noexcept override;
    void onEnd() noexcept override;

    path getResponseFilename() const;
    virtual String getResponseFileContents(bool showIncludes = false) const;
    int getFirstResponseFileArgument() const;

    Arguments &getArguments() override;
    const Arguments &getArguments() const override;

    Command &operator|(Command &);
    Command &operator|=(Command &);

    const SwBuilderContext &getContext() const;
    void setContext(const SwBuilderContext &);

protected:
    bool prepared = false;
    bool executed_ = false;

    virtual bool check_if_file_newer(const path &, const String &what, bool throw_on_missing) const;

private:
    const SwBuilderContext *swctx = nullptr;
    mutable size_t hash = 0;
    Arguments rsp_args;
    mutable String log_string;

    virtual void execute1(std::error_code *ec = nullptr);
    virtual size_t getHash1() const;

    void postProcess(bool ok = true);
    virtual void postProcess1(bool ok) {}

    bool beforeCommand();
    void afterCommand();
    bool isTimeChanged() const;
    void printLog() const;
    size_t getHashAndSave() const;
    String makeErrorString();
    String makeErrorString(const String &e);
    String saveCommand() const;
    void printOutputs();
};

struct SW_BUILDER_API CommandSequence : Command
{
    using Command::Command;

    void addCommand(const std::shared_ptr<Command> &c);

    template <class C = Command, class ... Args>
    std::shared_ptr<C> addCommand(Args && ... args)
    {
        auto c = std::make_shared<C>(getContext(), std::forward<Args>(args)...);
        commands.push_back(c);
        return c;
    }

    const std::vector<std::shared_ptr<Command>> &getCommands() { return commands; }

private:
    std::vector<std::shared_ptr<Command>> commands;

    void execute1(std::error_code *ec = nullptr) override;
    size_t getHash1() const override;
    void prepare() override;
};

// remove? probably no, just don't use it much
// we always can create executable commands that is not builtin into modules
struct SW_BUILDER_API ExecuteBuiltinCommand : Command
{
    using F = std::function<void(void)>;

    ExecuteBuiltinCommand(const SwBuilderContext &swctx);
    ExecuteBuiltinCommand(const SwBuilderContext &swctx, const String &cmd_name, void *f, int version = 0);
    virtual ~ExecuteBuiltinCommand() = default;

    using Command::push_back;
    void push_back(const Strings &strings);
    void push_back(const Files &files);

private:
    void execute1(std::error_code *ec = nullptr) override;
    size_t getHash1() const override;
    void prepare() override {}
};

SW_BUILDER_API
String getInternalCallBuiltinFunctionName();

} // namespace bulder

using builder::ExecuteBuiltinCommand;
using Commands = std::unordered_set<std::shared_ptr<builder::Command>>;

/// return input when file not found
SW_BUILDER_API
path resolveExecutable(const path &p);

SW_BUILDER_API
path resolveExecutable(const FilesOrdered &paths);

SW_BUILDER_API
std::map<path, String> &getMsvcIncludePrefixes();

SW_BUILDER_API
String detectMsvcPrefix(builder::detail::ResolvableCommand c, const path &idir);

// serialization

// remember to set context and command storage after loading
SW_BUILDER_API
Commands loadCommands(const path &archive_fn, int type = 0);

SW_BUILDER_API
void saveCommands(const path &archive_fn, const Commands &, int type = 0);

} // namespace sw

namespace std
{

template<> struct hash<sw::builder::Command>
{
    size_t operator()(const sw::builder::Command &c) const
    {
        return c.getHash();
    }
};

}
