// Copyright (C) 2017-2018 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "functions.h"

#include <sw/builder/file.h>

#include <primitives/hash.h>
#include <primitives/lock.h>
#include <primitives/patch.h>

#include <boost/algorithm/string.hpp>

#include <primitives/log.h>
DECLARE_STATIC_LOGGER(logger, "functions");

namespace sw
{

void writeFileOnce(const path &fn, const String &content, const path &lock_dir)
{
    auto h = sha1(content);

    auto hf = sha1(normalize_path(fn));
    const auto once = lock_dir / (hf + ".once");
    const auto lock = lock_dir / hf;

    if (!fs::exists(once) || h != read_file(once) || !fs::exists(fn))
    {
        ScopedFileLock fl(lock);
        write_file_if_different(fn, content);
        write_file_if_different(once, h);
    }
}

void writeFileSafe(const path &fn, const String &content, const path &lock_dir)
{
    auto hf = sha1(normalize_path(fn));
    const auto lock = lock_dir / hf;

    ScopedFileLock fl(lock);
    write_file_if_different(fn, content);
}

void replaceInFileOnce(const path &fn, const String &from, const String &to, const path &lock_dir)
{
    auto hf = sha1(normalize_path(fn));

    auto uniq = normalize_path(fn) + from + to;
    auto h = sha1(uniq).substr(0, 5);
    auto hfn = lock_dir / (hf + "." + h);

    if (fs::exists(hfn))
        return;

    const auto lock = lock_dir / hf;
    ScopedFileLock fl(lock);

    // double check
    if (fs::exists(hfn))
        return;

    auto s = read_file(fn);
    boost::replace_all(s, from, to);
    write_file_if_different(fn, s); // if different?
    write_file_if_different(hfn, "");
}

void pushFrontToFileOnce(const path &fn, const String &text, const path &lock_dir)
{
    auto hf = sha1(normalize_path(fn));

    auto uniq = normalize_path(fn) + text;
    auto h = sha1(uniq).substr(0, 5);
    auto hfn = lock_dir / (hf + "." + h);

    if (fs::exists(hfn))
        return;

    const auto lock = lock_dir / hf;
    ScopedFileLock fl(lock);

    // double check
    if (fs::exists(hfn))
        return;

    const auto orig = lock_dir / (hf + ".orig");
    if (!fs::exists(orig))
        fs::copy_file(fn, orig, fs::copy_options::overwrite_existing);
    else
        fs::copy_file(orig, fn, fs::copy_options::overwrite_existing);

    auto s = read_file(fn);
    s = text + "\n" + s;
    write_file_if_different(fn, s);
    write_file_if_different(hfn, "");
}

void pushBackToFileOnce(const path &fn, const String &text, const path &lock_dir)
{
    auto hf = sha1(normalize_path(fn));

    auto uniq = normalize_path(fn) + text;
    auto h = sha1(uniq).substr(0, 5);
    auto hfn = lock_dir / (hf + "." + h);

    if (fs::exists(hfn))
        return;

    const auto lock = lock_dir / hf;
    ScopedFileLock fl(lock);

    // double check
    if (fs::exists(hfn))
        return;

    const auto orig = lock_dir / (hf + ".orig");
    if (!fs::exists(orig))
        fs::copy_file(fn, orig, fs::copy_options::overwrite_existing);
    else
        fs::copy_file(orig, fn, fs::copy_options::overwrite_existing);

    auto s = read_file(fn);
    s = s + "\n" + text;
    write_file_if_different(fn, s);
    write_file_if_different(hfn, "");
}

bool patch(const path &fn, const String &patch, const path &lock_dir)
{
    auto t = read_file(fn);

    auto fn_patch = fn;
    fn_patch += ".orig." + sha1(normalize_path(fn)).substr(0, 8);

    if (fs::exists(fn_patch))
        return true;

    auto r = primitives::patch::patch(t, patch);
    if (!r.first)
    {
        //throw SW_RUNTIME_ERROR("cannot apply patch to: " + normalize_path(fn));
        LOG_ERROR(logger, "cannot apply patch to: " << fn << ", error: " << r.second);
        return false;
    }

    write_file(fn, r.second);
    write_file(fn_patch, t); // save orig

    return true;
}

}
