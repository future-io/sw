// Copyright (C) 2016-2019 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "settings.h"

#include <sw/support/hash.h>

namespace sw
{

String TargetSettings::getConfig() const
{
    String c;
    for (auto &[k, v] : *this)
        c += k + v;
    return c;
}

String TargetSettings::getHash() const
{
    return shorten_hash(blake2b_512(getConfig()), 6);
}

} // namespace sw