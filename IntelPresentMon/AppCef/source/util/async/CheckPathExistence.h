// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
#pragma once
#include "../AsyncEndpoint.h"
#include <Core/source/kernel/Kernel.h>
#include "../CefValues.h"
#include "../PathSanitaryCheck.h"


namespace p2c::client::util::async
{
    class CheckPathExistence : public AsyncEndpoint
    {
    public:
        static constexpr std::string GetKey() { return "checkPathExistence"; }
        CheckPathExistence() : AsyncEndpoint{ AsyncEndpoint::Environment::KernelTask } {}
        // {location:int, path:string} => bool
        Result ExecuteOnKernelTask(uint64_t uid, CefRefPtr<CefValue> pArgObj, kern::Kernel& kernel) const override
        {
            const auto filePath = ResolveSanitizedPath(
                Traverse(pArgObj)["location"],
                Traverse(pArgObj)["path"].AsWString());
            return Result{ true, MakeCefValue(std::filesystem::exists(filePath)) };
        }
    };
}