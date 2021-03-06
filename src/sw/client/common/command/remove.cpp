/*
 * SW - Build System and Package Manager
 * Copyright (C) 2017-2019 Egor Pugin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "commands.h"

#include <sw/manager/storage.h>

#include <primitives/log.h>
DECLARE_STATIC_LOGGER(logger, "remove");

static sw::PackageIdSet getMatchingPackagesSet(const sw::StorageWithPackagesDatabase &s, const String &unresolved_pkg)
{
    sw::PackageIdSet p;
    for (auto &[ppath, versions] : getMatchingPackages(s, unresolved_pkg))
    {
        for (auto &v : versions)
            p.emplace(ppath, v);
    }
    return p;
}

SUBCOMMAND_DECL(remove)
{
    auto swctx = createSwContext(options);
    for (auto &a : options.options_remove.remove_arg)
    {
        for (auto &p : getMatchingPackagesSet(swctx->getLocalStorage(), a))
        {
            LOG_INFO(logger, "Removing " << p.toString());
            swctx->getLocalStorage().remove(sw::LocalPackage(swctx->getLocalStorage(), p));
        }
    }
}
