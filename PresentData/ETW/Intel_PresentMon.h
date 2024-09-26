// Copyright (C) 2024 Intel Corporation
// SPDX-License-Identifier: MIT
//
// This file originally generated by etw_list
//     parameters: --no_event_structs --provider=Intel-PresentMon
#pragma once

namespace Intel_PresentMon {

struct __declspec(uuid("{ECAA4712-4644-442F-B94C-A32F6CF8A499}")) GUID_STRUCT;
static const auto GUID = __uuidof(GUID_STRUCT);

enum class Keyword : uint64_t {
    FrameTypes = 0x1,
};

enum class Level : uint8_t {
    win_Informational = 0x4,
};

// Event descriptors:
#define EVENT_DESCRIPTOR_DECL(name_, id_, version_, channel_, level_, opcode_, task_, keyword_) struct name_ { \
    static uint16_t const Id      = id_; \
    static uint8_t  const Version = version_; \
    static uint8_t  const Channel = channel_; \
    static uint8_t  const Level   = level_; \
    static uint8_t  const Opcode  = opcode_; \
    static uint16_t const Task    = task_; \
    static Keyword  const Keyword = (Keyword) keyword_; \
}

EVENT_DESCRIPTOR_DECL(AppInputSample_Info, 0x003a, 0x00, 0x00, 0x04, 0x00, 0x003a, 0x0000000000000004);
EVENT_DESCRIPTOR_DECL(AppPresentEnd_Info, 0x0039, 0x00, 0x00, 0x04, 0x00, 0x0039, 0x0000000000000004);
EVENT_DESCRIPTOR_DECL(AppPresentStart_Info, 0x0038, 0x00, 0x00, 0x04, 0x00, 0x0038, 0x0000000000000004);
EVENT_DESCRIPTOR_DECL(AppRenderSubmitEnd_Info, 0x0037, 0x00, 0x00, 0x04, 0x00, 0x0037, 0x0000000000000004);
EVENT_DESCRIPTOR_DECL(AppRenderSubmitStart_Info, 0x0036, 0x00, 0x00, 0x04, 0x00, 0x0036, 0x0000000000000004);
EVENT_DESCRIPTOR_DECL(AppSimulationEnd_Info, 0x0035, 0x00, 0x00, 0x04, 0x00, 0x0035, 0x0000000000000004);
EVENT_DESCRIPTOR_DECL(AppSimulationStart_Info, 0x0034, 0x00, 0x00, 0x04, 0x00, 0x0034, 0x0000000000000004);
EVENT_DESCRIPTOR_DECL(AppSleepEnd_Info, 0x0033, 0x00, 0x00, 0x04, 0x00, 0x0033, 0x0000000000000004);
EVENT_DESCRIPTOR_DECL(AppSleepStart_Info, 0x0032, 0x00, 0x00, 0x04, 0x00, 0x0032, 0x0000000000000004);
EVENT_DESCRIPTOR_DECL(FlipFrameType_Info, 0x0002, 0x00, 0x00, 0x04, 0x00, 0x0002, 0x0000000000000001);
EVENT_DESCRIPTOR_DECL(MeasuredInput_Info, 0x000a, 0x00, 0x00, 0x04, 0x00, 0x000a, 0x0000000000000002);
EVENT_DESCRIPTOR_DECL(MeasuredScreenChange_Info, 0x000b, 0x00, 0x00, 0x04, 0x00, 0x000b, 0x0000000000000002);
EVENT_DESCRIPTOR_DECL(PresentFrameType_Info, 0x0001, 0x00, 0x00, 0x04, 0x00, 0x0001, 0x0000000000000001);

#undef EVENT_DESCRIPTOR_DECL

enum class FrameType : uint8_t {
    Unspecified = 0,
    Original = 1,
    Repeated = 2,
    Intel_XEFG = 50,
    AMD_AFMF = 100,
};

#pragma pack(push, 1)

struct AppInputSample_Info_Props {
    uint32_t    FrameId;
    InputType   InputType;
};

struct AppPresentEnd_Info_Props {
    uint32_t    FrameId;
};

struct AppPresentStart_Info_Props {
    uint32_t    FrameId;
};

struct AppRenderSubmitEnd_Info_Props {
    uint32_t    FrameId;
};

struct AppRenderSubmitStart_Info_Props {
    uint32_t    FrameId;
};

struct AppSimulationEnd_Info_Props {
    uint32_t    FrameId;
};

struct AppSimulationStart_Info_Props {
    uint32_t    FrameId;
};

struct AppSleepEnd_Info_Props {
    uint32_t    FrameId;
};

struct AppSleepStart_Info_Props {
    uint32_t    FrameId;
};

struct MeasuredInput_Info_Props {
    uint64_t    Time;
    InputType   InputType;
};

struct MeasuredScreenChange_Info_Props {
    uint64_t    Time;
};

struct FlipFrameType_Info_Props {
    uint32_t VidPnSourceId;
    uint32_t LayerIndex;
    uint64_t PresentId;
    FrameType FrameType;
};

struct PresentFrameType_Info_Props {
    uint32_t FrameId;
    FrameType FrameType;
};

#pragma pack(pop)

}
