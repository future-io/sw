// Copyright (C) 2016-2018 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bazel
{

using Name = std::string;
using Value = std::string;
using Values = std::set<Value>;

struct Parameter
{
    Name name;
    Values values;

    void trimQuotes();
};

using Parameters = std::vector<Parameter>;

struct Function
{
    Name name;
    Parameters parameters;

    void trimQuotes();
};

using Functions = std::vector<Function>;

struct File
{
    Functions functions;
    std::unordered_map<Name, Parameter> parameters;

    void trimQuotes();
    Values getFiles(const Name &name, const std::string &bazel_target_function = std::string()) const;
};

File parse(const std::string &s);

} // namespace bazel
