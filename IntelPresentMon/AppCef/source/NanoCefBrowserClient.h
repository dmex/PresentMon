// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
#pragma once
#include <Core/source/win/WinAPI.h>
#include <include/cef_client.h>
#include <semaphore>
#include <memory>
#include <optional>
#include "util/AsyncEndpointCollection.h"


namespace p2c::client::cef
{
    class NanoCefBrowserClient :
        public CefClient,
        public CefLifeSpanHandler
    {
    public:
        NanoCefBrowserClient();
        CefRefPtr<CefBrowser> GetBrowser();
        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override;
        void OnAfterCreated(CefRefPtr<CefBrowser> browser_) override;
        void OnBeforeClose(CefRefPtr<CefBrowser> browser_) override;
        bool OnProcessMessageReceived(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            CefProcessId source_process,
            CefRefPtr<CefProcessMessage> message) override;
        std::optional<LRESULT> HandleCloseMessage();
        CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override;

    protected:
        // this semaphore protects from race condition between normal shutdown ack sequence and timeout fallback sequence
        std::binary_semaphore shutdownSemaphore{ 1 };
        // indicates whether ack was received from render process (or timeout fired)
        std::atomic<bool> shutdownAcknowledgementFlag = false;
        bool shutdownRequestFlag = false;

        CefRefPtr<CefContextMenuHandler> pContextMenuHandler;
        CefRefPtr<CefBrowser> pBrowser;
        util::AsyncEndpointCollection endpoints;

        // Include the default reference counting implementation.
        IMPLEMENT_REFCOUNTING(NanoCefBrowserClient);
    };
}