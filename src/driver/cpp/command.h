// Copyright (C) 2017-2018 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <sw/builder/command.h>

#include "options.h"

namespace sw::driver::cpp
{

struct SW_BUILDER_API Command : ::sw::builder::Command
{
    using Base = ::sw::builder::Command;

    std::shared_ptr<Dependency> dependency;

    path getProgram() const override;

    using Base::setProgram;
    void setProgram(const std::shared_ptr<Dependency> &d);
    void setProgram(const NativeTarget &t);
};

}