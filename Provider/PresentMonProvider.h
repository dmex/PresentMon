// Copyright (C) 2017-2024 Intel Corporation
// SPDX-License-Identifier: MIT
#pragma once

#ifndef PRESENTMONPROVIDER_ASSERT
#ifdef NDEBUG
#define PRESENTMONPROVIDER_ASSERT(_C) (void) (_C)
#else
#define PRESENTMONPROVIDER_ASSERT assert
#endif
#endif

// Before emitting any Intel-PresentMon events, the provider needs to be initialized by calling
// PresentMonProvider_Initialize().  PresentMonProvider_Initialize() returns a context to use for
// other PresentMonPrivider calls, or nullptr if the initialization failed.
//
// Call PresentMonProvider_ShutDown() when you no longer need to generate any Intel-PresentMon
// events.  ctxt is deallocated during PresentMonProvider_ShutDown() and can no longer be used.

struct PresentMonProvider;
PresentMonProvider* PresentMonProvider_Initialize();

void PresentMonProvider_ShutDown(PresentMonProvider* ctxt);


// Once you successfully initialize the provider, you can communicate with PresentMon using the
// following API.
//
// Function arguments are checked with PRESENTMONPROVIDER_ASSERT().  Each function will return one
// of the following status values:
//
//     - ERROR_SUCCESS:             The event was written successfully.
//
//     - ERROR_INVALID_HANDLE:      PresentMonProvider_Initialize() was not called or was not
//                                  successful.
//
//     - ERROR_MORE_DATA:           The ETW session buffer size is too small for the event.
//
//     - ERROR_NOT_ENOUGH_MEMORY:   The disk IO could not keep up with the amount of events being
//                                  generated.
//
//     - Other return values may be returned in the event of a PresentMonProvider internal error.


// PRESENTING GENERATED FRAMES
//
// Drivers or SDKs that present generated frames other than the application-rendered frames should
// use these functions to inform PresentMon what type of frame is being presented.
//
// Use PresentMonProvider_PresentFrameType() when presenting generated frames that are submitted
// through a standard present API. PresentMonProvider_PresentFrameType() should be called before
// calling each Present(), on the same thread as the Present() call.  Use 'frameId' to identify when
// multiple Present()s refer to the same logical frame.  e.g., you may have one
// PresentMonProvider_FrameType_Original Present() and multiple
// non-PresentMonProvider_FrameType_Original Present()s associated with the same frameId.  Do not
// reuse a frameId value to refer to a new frame for at least several seconds.
//
// Use PresentMonProvider_FlipFrameType() when displaying generated frames that are submitted
// through a proprietary mechanism. PresentMonProvider_FlipFrameType() should be called as close as
// possible to the time the frame is displayed. 'vidPnSourceId', 'layerIndex', and 'presentId' refer
// to the application's initially-presented frame and should match the values in the corresponding
// SetVidPnSourceAddressWithMultiPlaneOverlay3 call.

enum PresentMonProvider_FrameType {
    PresentMonProvider_FrameType_Unspecified,       // Use when no other values are appropriate (file github request
                                                    // to have your technique added).
    PresentMonProvider_FrameType_Original,          // The original frame rendered by the application.
    PresentMonProvider_FrameType_Repeated,          // The frame rendered by the application is being repeated.
    PresentMonProvider_FrameType_Intel_XEFG = 50,   // Frame generated by Intel Xe Frame Generation.
    PresentMonProvider_FrameType_AMD_AFMF = 100,    // Frame generated by AMD Fluid Motion Frames.
};

ULONG PresentMonProvider_PresentFrameType(PresentMonProvider* ctxt,
                                          uint32_t frameId,
                                          PresentMonProvider_FrameType frameType);

ULONG PresentMonProvider_FlipFrameType(PresentMonProvider* ctxt,
                                       uint32_t vidPnSourceId,
                                       uint32_t layerIndex,
                                       uint64_t presentId,
                                       PresentMonProvider_FrameType frameType);

// MEASURED INPUT/PHOTON LATENCY
//
// These provide times associated with user input and monitor updates that were measured using some
// external method which can be used to include hardware latency within the input and display
// devices as part of PresentMon's latency calculations.
//
// Try to call PresentMonProvider_Measured*() as quickly as possible after the measurment is
// available.  If PresentMon doesn't observe the measured data after waiting for some period, it may
// flush out it's SW-measured values instead.
//
// Input/display times are provided as QueryPerformanceCounter (QPC) values.  Special care must be
// taken to correlate the measurement device's measured time with the PC's QPC as closely as
// possible.

enum PresentMonProvider_InputType {
    PresentMonProvider_Input_NotSpecified   = 0,
    PresentMonProvider_Input_MouseClick     = 1 << 0,
    PresentMonProvider_Input_KeyboardClick  = 1 << 1,
};

ULONG PresentMonProvider_MeasuredInput(PresentMonProvider* ctxt,
                                       PresentMonProvider_InputType inputType,
                                       uint64_t inputQPCTime);

ULONG PresentMonProvider_MeasuredScreenChange(PresentMonProvider* ctxt,
                                              uint64_t screenQPCTime);

// GRAPHICS APPLICATION FRAME INFORMATION

ULONG PresentMonProvider_Application_SleepStart(PresentMonProvider* ctxt,
                                                uint32_t frame_id);

ULONG PresentMonProvider_Application_SleepEnd(PresentMonProvider* ctxt,
                                              uint32_t frame_id);

ULONG PresentMonProvider_Application_SimulationStart(PresentMonProvider* ctxt,
                                                     uint32_t frame_id);

ULONG PresentMonProvider_Application_SimulationEnd(PresentMonProvider* ctxt,
                                                   uint32_t frame_id);

ULONG PresentMonProvider_Application_RenderSubmitStart(PresentMonProvider* ctxt,
                                                       uint32_t frame_id);

ULONG PresentMonProvider_Application_RenderSubmitEnd(PresentMonProvider* ctxt,
                                                     uint32_t frame_id);

ULONG PresentMonProvider_Application_PresentStart(PresentMonProvider* ctxt,
                                                  uint32_t frame_id);

ULONG PresentMonProvider_Application_PresentEnd(PresentMonProvider* ctxt,
                                                uint32_t frame_id);

ULONG PresentMonProvider_Application_InputSample(PresentMonProvider* ctxt,
                                                 PresentMonProvider_InputType inputType,
                                                 uint32_t frame_id);