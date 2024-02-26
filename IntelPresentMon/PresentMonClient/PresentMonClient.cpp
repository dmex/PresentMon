// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
#include <span>
#include <format>
#include "PresentMonClient.h"
#include "../PresentMonUtils/QPCUtils.h"
#include "../PresentMonUtils/PresentDataUtils.h"
#include <iostream>
#include <filesystem>
#include <format>
#include <algorithm>
#include <shlobj.h>
#include "../CommonUtilities/source/str/String.h"
#include "../PresentMonService/GlobalIdentifiers.h"

#define GOOGLE_GLOG_DLL_DECL
#define GLOG_NO_ABBREVIATED_SEVERITIES
#include <glog/logging.h>

static const uint32_t kMaxRespBufferSize = 4096;
static const uint64_t kClientFrameDeltaQPCThreshold = 50000000;

using namespace pmon;

void InitializeLogging(const char* location, const char* basename, const char* extension, int level)
{
    // initialize glog based on command line if not already initialized
    // clients using glog should init before initializing pmon api
    if (!google::IsGoogleLoggingInitialized()) {
        google::InitGoogleLogging(__argv[0]);
        if (!basename) {
            basename = "pmon-sdk";
        }
        if (location) {
            google::SetLogDestination(google::GLOG_INFO, std::format("{}\\{}-info-", location, basename).c_str());
            google::SetLogDestination(google::GLOG_WARNING, std::format("{}\\{}-warn-", location, basename).c_str());
            google::SetLogDestination(google::GLOG_ERROR, std::format("{}\\{}-err-", location, basename).c_str());
            google::SetLogDestination(google::GLOG_FATAL, std::format("{}\\{}-fatal-", location, basename).c_str());
        }
        if (extension) {
            google::SetLogFilenameExtension(extension);
        }
        FLAGS_minloglevel = std::clamp(level, 0, 3);
        LOG(INFO) << "hello from log init" << std::endl;
    }
}

PresentMonClient::PresentMonClient(const char* controlPipeName)
    : pipe_(INVALID_HANDLE_VALUE),
      set_metric_offset_in_qpc_ticks_(0),
      client_to_frame_data_delta_(0) {

    std::wstring pipe_name = controlPipeName ?
        util::str::ToWide(controlPipeName) :
        util::str::ToWide(gid::defaultControlPipeName);
  
  // Try to open a named pipe; wait for it, if necessary.
  while (1) {
    pipe_ = CreateFile(pipe_name.c_str(),      // pipe name
                       GENERIC_READ |  // read and write access
                           GENERIC_WRITE,
                       0,              // no sharing
                       NULL,           // default security attributes
                       OPEN_EXISTING,  // opens existing pipe
                       0,              // default attributes
                       NULL);          // no template file

    // Break if the pipe handle is valid.
    if (pipe_ != INVALID_HANDLE_VALUE) {
      break;
    }

    // Exit if an error other than ERROR_PIPE_BUSY occurs.
    if (const auto hr = GetLastError(); hr != ERROR_PIPE_BUSY) {
      LOG(ERROR) << "Service not found; errno: " << hr << std::endl;
      throw std::runtime_error{"Service not found"};
    }

    // All pipe instances are busy, so wait for 20 seconds.
    if (!WaitNamedPipe(pipe_name.c_str(), 20000)) {
      LOG(ERROR) << "Pipe sessions full." << std::endl;
      throw std::runtime_error{"Pipe sessions full"};
    }
  }
  // The pipe connected; change to message-read mode.
  DWORD mode = PIPE_READMODE_MESSAGE;
  BOOL success = SetNamedPipeHandleState(pipe_,  // pipe handle
                                         &mode,  // new pipe mode
                                         NULL,   // don't set maximum bytes
                                         NULL);  // don't set maximum time
  if (!success) {
    LOG(ERROR) << "Pipe error." << std::endl;
    throw std::runtime_error{"Pipe error"};
  }

  // Store the client process id
  client_process_id_ = GetCurrentProcessId();
}

PresentMonClient::~PresentMonClient() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  return;
}

// Calculate percentile using linear interpolation between the closet ranks
// data must be pre-sorted
double PresentMonClient::GetPercentile(std::vector<double>& data,
                                       double percentile) {

  percentile = max(percentile, 0.);

  double integral_part_as_double;
  double fractpart =
      modf(percentile * static_cast<double>(data.size()),
           &integral_part_as_double);

  uint32_t idx = static_cast<uint32_t>(integral_part_as_double);
  if (idx >= data.size() - 1) {
    return data[data.size() - 1];
  }

  return data[idx] + (fractpart * (data[idx + 1] - data[idx]));
}

PM_STATUS PresentMonClient::GetGfxLatencyData(
    uint32_t process_id, PM_GFX_LATENCY_DATA* gfx_latency_data,
    double window_size_in_ms, uint32_t* num_swapchains) {
  std::unordered_map<uint64_t, fps_swap_chain_data> swap_chain_data;

  if (*num_swapchains == 0) {
    return PM_STATUS::PM_STATUS_FAILURE;
  }

  auto iter = clients_.find(process_id);
  if (iter == clients_.end()) {
    LOG(INFO) << "Stream client for process " << process_id
              << " doesn't exist. Please call pmStartStream to initialize the "
                 "client.";
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }

  StreamClient* client = iter->second.get();
  auto nsm_view = client->GetNamedSharedMemView();
  auto nsm_hdr = nsm_view->GetHeader();
  if (!nsm_hdr->process_active) {
    // Server destroyed the named shared memory due to process exit. Destroy
    // the mapped view from client side.
    StopStreamProcess(process_id);
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }

  auto cache_iter = client_metric_caches_.find(process_id);
  if (cache_iter == client_metric_caches_.end()) {
    // This should never happen!
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }
  auto metric_cache = &cache_iter->second;

  LARGE_INTEGER qpc_frequency = client->GetQpcFrequency();

  uint64_t index = 0;
  PmNsmFrameData* frame_data =
      GetFrameDataStart(client, index, window_size_in_ms);
  if (frame_data == nullptr) {
    if (CopyCacheData(gfx_latency_data, num_swapchains, metric_cache->cached_latency_data_)) {
      return PM_STATUS::PM_STATUS_SUCCESS;
    } else {
      return PM_STATUS::PM_STATUS_NO_DATA;
    }
  }

  // Calculate the end qpc based on the current frame's qpc and
  // requested window size coverted to a qpc
  uint64_t end_qpc =
      frame_data->present_event.PresentStartTime -
      ContvertMsToQpcTicks(window_size_in_ms, qpc_frequency.QuadPart);

  // Loop from the most recent frame data until we either run out of data or we
  // meet the window size requirements sent in by the client
  while (frame_data->present_event.PresentStartTime > end_qpc) {
    auto result = swap_chain_data.emplace(
        frame_data->present_event.SwapChainAddress, fps_swap_chain_data());
    auto swap_chain = &result.first->second;
    if (result.second) {
      swap_chain->cpu_n_time = frame_data->present_event.PresentStartTime;
      swap_chain->cpu_0_time = frame_data->present_event.PresentStartTime;
      swap_chain->display_latency_ms.clear();
      swap_chain->render_latency_ms.clear();
      swap_chain->num_presents = 1;
    } else {
      if (frame_data->present_event.FinalState == PresentResult::Presented) {
          double current_render_latency_ms = 0.0;
          double current_display_latency_ms = 0.0;

          if (frame_data->present_event.ScreenTime >
              frame_data->present_event.ReadyTime) {
            current_display_latency_ms =
                QpcDeltaToMs(frame_data->present_event.ScreenTime -
                                 frame_data->present_event.ReadyTime,
                             qpc_frequency);
            swap_chain->display_latency_sum +=
                frame_data->present_event.ScreenTime -
                frame_data->present_event.ReadyTime;
          } else {
            current_display_latency_ms =
                -1.0 * QpcDeltaToMs(frame_data->present_event.ReadyTime -
                                        frame_data->present_event.ScreenTime,
                                    qpc_frequency);
            swap_chain->display_latency_sum +=
                (-1 * (frame_data->present_event.ReadyTime -
                       frame_data->present_event.ScreenTime));
          }
          swap_chain->display_latency_ms.push_back(current_display_latency_ms);

          current_render_latency_ms =
              QpcDeltaToMs(frame_data->present_event.ScreenTime -
                               frame_data->present_event.PresentStartTime,
                           qpc_frequency);
          swap_chain->render_latency_ms.push_back(current_render_latency_ms);
          swap_chain->render_latency_sum +=
              frame_data->present_event.ScreenTime -
              frame_data->present_event.PresentStartTime;
          swap_chain->display_count++;
      }
      swap_chain->cpu_0_time = frame_data->present_event.PresentStartTime;
      swap_chain->num_presents++;
    }

    // Get the index of the next frame
    if (DecrementIndex(nsm_view, index) == false) {
      // We have run out of data to process, time to go
      break;
    }
    frame_data = client->ReadFrameByIdx(index);
    if (frame_data == nullptr) {
      break;
    }
  }

  PM_GFX_LATENCY_DATA* current_gfx_latency_data = gfx_latency_data;
  uint32_t gfx_latency_data_counter = 0;
  for (auto pair : swap_chain_data) {
    ZeroMemory(current_gfx_latency_data, sizeof(PM_GFX_LATENCY_DATA));

    // Save off the current swap chain
    current_gfx_latency_data->swap_chain = pair.first;

    auto chain = pair.second;

    CalculateMetricDoubleData(chain.display_latency_ms,
                              current_gfx_latency_data->display_latency_ms);
    CalculateMetricDoubleData(chain.render_latency_ms,
                              current_gfx_latency_data->render_latency_ms);

    // There are couple reasons where we will not be able to produce
    // fps metric data. The first is if all of the frames are dropped.
    // The second is if in the requested sample window there are
    // no presents.
    if ((chain.display_count <= 1) && (chain.num_presents <= 1)) {
      if (gfx_latency_data_counter < metric_cache->cached_latency_data_.size()) {
        *current_gfx_latency_data =
            metric_cache->cached_latency_data_[gfx_latency_data_counter];
      }
    } else {
      if (gfx_latency_data_counter >=
          metric_cache->cached_latency_data_.size()) {
        metric_cache->cached_latency_data_.push_back(*current_gfx_latency_data);
      } else {
        metric_cache->cached_latency_data_[gfx_latency_data_counter] =
            *current_gfx_latency_data;
      }
    }

    gfx_latency_data_counter++;
    if (gfx_latency_data_counter >= *num_swapchains) {
      // Reached the amount of fp data structures passed in.
      break;
    }
    current_gfx_latency_data++;
  }

  // Update the number of swap chains available so client can adjust
  *num_swapchains = static_cast<uint32_t>(swap_chain_data.size());
  return PM_STATUS::PM_STATUS_SUCCESS;
}

PM_STATUS PresentMonClient::GetFramesPerSecondData(uint32_t process_id,
                                                   PM_FPS_DATA* fps_data,
                                                   double window_size_in_ms,
                                                   uint32_t* num_swapchains) {
  std::unordered_map<uint64_t, fps_swap_chain_data> swap_chain_data;
  LARGE_INTEGER api_qpc;
  QueryPerformanceCounter(&api_qpc);
  
  if (*num_swapchains == 0) {
    return PM_STATUS::PM_STATUS_FAILURE;
  }

  auto iter = clients_.find(process_id);
  if (iter == clients_.end()) {
    LOG(INFO) << "Stream client for process " << process_id
              << " doesn't exist. Please call pmStartStream to initialize the "
                 "client.";
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }

  StreamClient* client = iter->second.get();
  auto nsm_view = client->GetNamedSharedMemView();
  auto nsm_hdr = nsm_view->GetHeader();
  if (!nsm_hdr->process_active) {
    // Server destroyed the named shared memory due to process exit. Destroy the
    // mapped view from client side.
    StopStreamProcess(process_id);
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }

  auto cache_iter = client_metric_caches_.find(process_id);
  if (cache_iter == client_metric_caches_.end()) {
    // This should never happen!
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }
  auto metric_cache = &cache_iter->second;

  LARGE_INTEGER qpc_frequency = client->GetQpcFrequency();

  uint64_t index = 0;
  double adjusted_window_size_in_ms = window_size_in_ms;
  PmNsmFrameData* frame_data =
      GetFrameDataStart(client, index, adjusted_window_size_in_ms);
  if (frame_data == nullptr) {
    if (CopyCacheData(fps_data, num_swapchains, metric_cache->cached_fps_data_)) {
      return PM_STATUS::PM_STATUS_SUCCESS;
    } else {
      return PM_STATUS::PM_STATUS_NO_DATA;
    }
  }

  // Calculate the end qpc based on the current frame's qpc and
  // requested window size coverted to a qpc
  uint64_t end_qpc =
      frame_data->present_event.PresentStartTime -
      ContvertMsToQpcTicks(adjusted_window_size_in_ms, qpc_frequency.QuadPart);

  // These are only used for logging:
  uint64_t last_checked_qpc = frame_data->present_event.PresentStartTime;
  bool decrement_failed = false;
  bool read_frame_failed = false;

  // Loop from the most recent frame data until we either run out of data or
  // we meet the window size requirements sent in by the client
  while (frame_data->present_event.PresentStartTime > end_qpc) {
    auto result = swap_chain_data.emplace(
        frame_data->present_event.SwapChainAddress, fps_swap_chain_data());
    auto swap_chain = &result.first->second;

    // Copy swap_chain data for the previous first frame needed for calculations below
    auto nextFramePresentStartTime     = swap_chain->cpu_0_time;
    auto nextFramePresentStopTime      = swap_chain->present_stop_0;
    auto nextFrameGPUDuration          = swap_chain->gpu_duration_0;
    auto nextFrameDisplayDuration      = swap_chain->display_1_screen_time - swap_chain->display_0_screen_time;
    auto nextFrameDisplayDurationValid = swap_chain->displayed_0 && swap_chain->display_count >= 2;

    // Save current frame's properties into swap_chain (the new first frame)
    swap_chain->displayed_0    = frame_data->present_event.FinalState == PresentResult::Presented;
    swap_chain->cpu_0_time     = frame_data->present_event.PresentStartTime;
    swap_chain->present_stop_0 = frame_data->present_event.PresentStartTime + frame_data->present_event.TimeInPresent;
    swap_chain->gpu_duration_0 = frame_data->present_event.GPUDuration;
    swap_chain->num_presents   += 1;

    if (swap_chain->displayed_0) {
      swap_chain->display_1_screen_time = swap_chain->display_0_screen_time;
      swap_chain->display_0_screen_time = frame_data->present_event.ScreenTime;
      swap_chain->display_count         += 1;
      if (swap_chain->display_count == 1) {
        swap_chain->display_n_screen_time = frame_data->present_event.ScreenTime;
      }
    }

    // These are only saved for the last frame:
    if (swap_chain->num_presents == 1) {
      swap_chain->cpu_n_time     = frame_data->present_event.PresentStartTime;
      swap_chain->sync_interval  = frame_data->present_event.SyncInterval;
      swap_chain->present_mode   = TranslatePresentMode(frame_data->present_event.PresentMode);
      swap_chain->allows_tearing = static_cast<int32_t>(frame_data->present_event.SupportsTearing);
    }

    // Compute metrics for this frame if we've seen enough subsequent frames to have all the
    // required data
    //
    // frame_data:      PresentStart--PresentStop--GPUDuration--ScreenTime
    // nextFrame:                                   PresentStart--PresentStop--GPUDuration--ScreenTime
    // nextNextFrame:                                                           PresentStart--PresentStop--GPUDuration--ScreenTime
    //                                CPUStart
    //                                CPUBusy------>CPUWait----------------->  GPUBusy--->  DisplayBusy---------------->
    if (swap_chain->num_presents > 1) {
      auto cpuStart    = frame_data->present_event.PresentStartTime + frame_data->present_event.TimeInPresent;
      auto cpuBusy     = nextFramePresentStartTime - cpuStart;
      auto cpuWait     = nextFramePresentStopTime - nextFramePresentStartTime;
      auto gpuBusy     = nextFrameGPUDuration;
      auto displayBusy = nextFrameDisplayDuration;

      auto frameTime_ms   = QpcDeltaToMs(cpuBusy + cpuWait, qpc_frequency);
      auto gpuBusy_ms     = QpcDeltaToMs(gpuBusy,           qpc_frequency);
      auto displayBusy_ms = QpcDeltaToMs(displayBusy,       qpc_frequency);

      swap_chain->frame_times_ms.push_back(frameTime_ms);
      swap_chain->gpu_sum_ms.push_back(gpuBusy_ms);
      swap_chain->dropped.push_back(swap_chain->displayed_0 ? 0. : 1.);

      if (nextFrameDisplayDurationValid && displayBusy > 0) {
        // TODO(megalvan): displayBusy > 0 because observed cases with the same screen time for
        // back to back frames.
        swap_chain->displayed_fps.push_back(1000. / displayBusy_ms);
      }
    }

    // Get the index of the next frame
    if (DecrementIndex(nsm_view, index) == false) {
      // We have run out of data to process, time to go
      decrement_failed = true;
      break;
    }
    frame_data = client->ReadFrameByIdx(index);
    if (frame_data == nullptr) {
      read_frame_failed = true;
      break;
    }
  }

  PM_FPS_DATA* current_fps_data = fps_data;
  uint32_t fps_data_counter = 0;
  for (auto pair : swap_chain_data) {
    
    ZeroMemory(current_fps_data, sizeof(PM_FPS_DATA));

    // Save off the current swap chain
    current_fps_data->swap_chain = pair.first;
    auto chain = pair.second;

    // Calculate the display fps metrics.
    CalculateMetricDoubleDataNoAvg(chain.displayed_fps,
                                   current_fps_data->displayed_fps);
    // Calculate the average using the screen times
    if (chain.display_count > 1) {
      current_fps_data->displayed_fps.avg = QpcDeltaToMs(
          chain.display_n_screen_time - chain.display_0_screen_time,
          qpc_frequency);
      current_fps_data->displayed_fps.avg /= chain.display_count;
      current_fps_data->displayed_fps.avg =
          (current_fps_data->displayed_fps.avg > 0.0)
              ? 1000.0 / current_fps_data->displayed_fps.avg
              : 0.0;
    }

    // Calculate stats for frametimes
    CalculateMetricDoubleDataNoAvg(chain.frame_times_ms,
                                   current_fps_data->frame_time_ms);
    if (chain.num_presents > 1) {
      current_fps_data->frame_time_ms.avg =
          QpcDeltaToMs(chain.cpu_n_time - chain.cpu_0_time, qpc_frequency);
      current_fps_data->frame_time_ms.avg =
          current_fps_data->frame_time_ms.avg / chain.num_presents;

      // TODO: The below percentiles should be 0.01,0.05,0.10 but I'm leaving it wrong so that it
      // matches the current CalculateMetricDoubleData() implementation.

      auto MillisecondsToFPS = [](double ms) { return ms == 0. ? 0. : 1000. / ms; };
      current_fps_data->presented_fps.avg           = MillisecondsToFPS(current_fps_data->frame_time_ms.avg);
      current_fps_data->presented_fps.raw           = MillisecondsToFPS(current_fps_data->frame_time_ms.raw);
      current_fps_data->presented_fps.low           = MillisecondsToFPS(current_fps_data->frame_time_ms.high);
      current_fps_data->presented_fps.high          = MillisecondsToFPS(current_fps_data->frame_time_ms.low);
      current_fps_data->presented_fps.percentile_99 = MillisecondsToFPS(GetPercentile(chain.frame_times_ms, 0.99));
      current_fps_data->presented_fps.percentile_95 = MillisecondsToFPS(GetPercentile(chain.frame_times_ms, 0.95));
      current_fps_data->presented_fps.percentile_90 = MillisecondsToFPS(GetPercentile(chain.frame_times_ms, 0.90));
    } else {
      current_fps_data->presented_fps.avg = 0.;
      current_fps_data->presented_fps.raw = 0.;
      current_fps_data->presented_fps.low = 0.;
      current_fps_data->presented_fps.high = 0.;
      current_fps_data->presented_fps.percentile_99 = 0.;
      current_fps_data->presented_fps.percentile_95 = 0.;
      current_fps_data->presented_fps.percentile_90 = 0.;
    }
    current_fps_data->presented_fps.valid = true;

    // Calculate stats for gpu busy
    CalculateMetricDoubleData(chain.gpu_sum_ms, current_fps_data->gpu_busy);
    // Calculate stats for dropped frames and then adjust the the avg
    // to be a percentage
    CalculateMetricDoubleData(chain.dropped,
                              current_fps_data->percent_dropped_frames);
    current_fps_data->percent_dropped_frames.avg *= 100.;

    // We don't record each frame's sync interval, present
    // mode or tearing status. Instead we take the values from
    // the more recent frame
    current_fps_data->sync_interval = chain.sync_interval;
    current_fps_data->present_mode = chain.present_mode;
    current_fps_data->allows_tearing = chain.allows_tearing;
    current_fps_data->num_presents = chain.num_presents;

    // There are couple reasons where we will not be able to produce
    // fps metric data. The first is if all of the frames are dropped.
    // The second is if in the requested sample window there are
    // no presents.
    bool using_cache = false;
    if ((chain.display_count <= 1) && (chain.num_presents <= 1)) {
      if (fps_data_counter < metric_cache->cached_fps_data_.size()) {
        *current_fps_data = metric_cache->cached_fps_data_[fps_data_counter];
        using_cache = true;
      }
    } else {
      if (fps_data_counter >= metric_cache->cached_fps_data_.size()) {
        metric_cache->cached_fps_data_.push_back(*current_fps_data);
      } else {
        metric_cache->cached_fps_data_[fps_data_counter] = *current_fps_data;
      }
    }

    // Only enable cache logging when the service has been started with
    // appropriate start parameters
    if (enable_file_logging_) {
      LOG(INFO) << api_qpc.QuadPart << "," << pair.first << ","
                << chain.num_presents << "," << chain.display_count << ","
                << chain.cpu_n_time << "," << chain.cpu_0_time << "," << end_qpc
                << "," << last_checked_qpc << "," << chain.display_n_screen_time
                << "," << chain.display_0_screen_time << ","
                << adjusted_window_size_in_ms << ","
                << (1000. / current_fps_data->frame_time_ms.avg) << ","
                << current_fps_data->frame_time_ms.raw << ","
                << current_fps_data->displayed_fps.raw << ","
                << decrement_failed << ","
                << read_frame_failed << "," << using_cache;
    }

    fps_data_counter++;
    if (fps_data_counter >= *num_swapchains) {
      // Reached the amount of fp data structures passed in.
      break;
    }
    current_fps_data++;
  }

  // Update the number of swap chains available so client can adjust
  *num_swapchains = static_cast<uint32_t>(swap_chain_data.size());
  return PM_STATUS::PM_STATUS_SUCCESS;
}

PM_STATUS PresentMonClient::SendRequest(MemBuffer* rqst_buf) {
  DWORD bytes_written;

  BOOL success = WriteFile(
      pipe_,                                           // pipe handle
      rqst_buf->AccessMem(),                           // message
      static_cast<DWORD>(rqst_buf->GetCurrentSize()),  // message length
      &bytes_written,                                  // bytes written
      NULL);                                           // not overlapped

  if (success && rqst_buf->GetCurrentSize() == bytes_written) {
    return PM_STATUS::PM_STATUS_SUCCESS;
  } else {
    return TranslateGetLastErrorToPmStatus(GetLastError());
  }
}

PM_STATUS PresentMonClient::ReadResponse(MemBuffer* rsp_buf) {
  BOOL success;
  DWORD bytes_read;
  BYTE in_buffer[kMaxRespBufferSize];
  ZeroMemory(&in_buffer, sizeof(in_buffer));

  do {
    // Read from the pipe using a nonoverlapped read
    success = ReadFile(pipe_,              // pipe handle
                       in_buffer,          // buffer to receive reply
                       sizeof(in_buffer),  // size of buffer
                       &bytes_read,        // number of bytes read
                       NULL);              // not overlapped

    // If the call was not successful AND there was
    // no more data to read bail out
    if (!success && GetLastError() != ERROR_MORE_DATA) {
      break;
    }

    // Either the call was successful or there was more
    // data in the pipe. In both cases add the response data
    // to the memory buffer
    rsp_buf->AddItem(in_buffer, bytes_read);
  } while (!success);  // repeat loop if ERROR_MORE_DATA

  if (success) {
    return PM_STATUS::PM_STATUS_SUCCESS;
  } else {
    return TranslateGetLastErrorToPmStatus(GetLastError());
  }
}

PM_STATUS PresentMonClient::CallPmService(MemBuffer* rqst_buf,
                                          MemBuffer* rsp_buf) {
  PM_STATUS status;

  status = SendRequest(rqst_buf);
  if (status != PM_STATUS::PM_STATUS_SUCCESS) {
    return status;
  }

  status = ReadResponse(rsp_buf);
  if (status != PM_STATUS::PM_STATUS_SUCCESS) {
    return status;
  }

  return status;
}

PM_STATUS PresentMonClient::RequestStreamProcess(uint32_t process_id) {
  MemBuffer rqst_buf;
  MemBuffer rsp_buf;

  NamedPipeHelper::EncodeStartStreamingRequest(&rqst_buf, client_process_id_,
                                               process_id, nullptr);

  PM_STATUS status = CallPmService(&rqst_buf, &rsp_buf);
  if (status != PM_STATUS::PM_STATUS_SUCCESS) {
    return status;
  }

  IPMSMStartStreamResponse start_stream_response{};

  status = NamedPipeHelper::DecodeStartStreamingResponse(
      &rsp_buf, &start_stream_response);
  if (status != PM_STATUS::PM_STATUS_SUCCESS) {
    return status;
  }

  string mapfile_name(start_stream_response.fileName);
  enable_file_logging_ = start_stream_response.enable_file_logging;

  // Initialize client with returned mapfile name
  auto iter = clients_.find(process_id);
  if (iter == clients_.end()) {
    try {
      std::unique_ptr<StreamClient> client =
          std::make_unique<StreamClient>(std::move(mapfile_name), false);

      clients_.emplace(process_id, std::move(client));
    } catch (...) {
      LOG(ERROR) << "Unabled to add client.\n";
      return PM_STATUS::PM_STATUS_FAILURE;
    }
  }

  if (!SetupClientCaches(process_id)) {
    LOG(ERROR) << "Unabled to setup client metric caches.\n";
    return PM_STATUS::PM_STATUS_FAILURE;
  }

  if (enable_file_logging_) {
    InitializeClientLogging();
  }

  return status;
}

PM_STATUS PresentMonClient::RequestStreamProcess(char const* etl_file_name) {
    MemBuffer rqst_buf;
    MemBuffer rsp_buf;

    NamedPipeHelper::EncodeStartStreamingRequest(
        &rqst_buf, client_process_id_,
        static_cast<uint32_t>(StreamPidOverride::kEtlPid), etl_file_name);

    PM_STATUS status = CallPmService(&rqst_buf, &rsp_buf);
    if (status != PM_STATUS::PM_STATUS_SUCCESS) {
        return status;
    }

    IPMSMStartStreamResponse start_stream_response{};

    status = NamedPipeHelper::DecodeStartStreamingResponse(
        &rsp_buf, &start_stream_response);
    if (status != PM_STATUS::PM_STATUS_SUCCESS) {
        return status;
    }

    string mapfile_name(start_stream_response.fileName);
    enable_file_logging_ = start_stream_response.enable_file_logging;

    try {
        etl_client_ = std::make_unique<StreamClient>(mapfile_name, true);
    } catch (...) {
        LOG(ERROR) << "Unabled to create stream client.\n";
        return PM_STATUS::PM_STATUS_FAILURE;
    }

    return status;
}

PM_STATUS PresentMonClient::StopStreamProcess(uint32_t process_id) {
    MemBuffer rqst_buf;
    MemBuffer rsp_buf;

    NamedPipeHelper::EncodeStopStreamingRequest(&rqst_buf,
                                                client_process_id_,
                                                process_id);

    PM_STATUS status = CallPmService(&rqst_buf, &rsp_buf);
    if (status != PM_STATUS::PM_STATUS_SUCCESS) {
        return status;
    }

    status = NamedPipeHelper::DecodeStopStreamingResponse(&rsp_buf);
    if (status != PM_STATUS::PM_STATUS_SUCCESS) {
        return status;
    }

    // Remove client
    {
        auto iter = clients_.find(process_id);
        if (iter != clients_.end()) {
      clients_.erase(std::move(iter));
        }
    }

    // Remove process metric cache
    RemoveClientCaches(process_id);

    return status;
}

PM_STATUS PresentMonClient::GetGpuData(uint32_t process_id,
                                       PM_GPU_DATA* gpu_data,
                                       double window_size_in_ms) {
  std::vector<double> gpu_telemetry_items[static_cast<size_t>(
      GpuTelemetryCapBits::gpu_telemetry_count)];
  std::vector<double> psu_voltage[MAX_PM_PSU_COUNT];
  std::vector<double> gpu_mem_utilization;
  
  auto iter = clients_.find(process_id);

  if (iter == clients_.end()) {
    LOG(INFO) << "Stream client for process " << process_id
              << " doesn't exist. Please call pmStartStream to initialize the "
                 "client.";
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }

  StreamClient* client = iter->second.get();
  auto nsm_view = client->GetNamedSharedMemView();
  LARGE_INTEGER qpc_frequency = client->GetQpcFrequency();

  auto cache_iter = client_metric_caches_.find(process_id);
  if (cache_iter == client_metric_caches_.end()) {
    // This should never happen!
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }
  auto metric_cache = &cache_iter->second;

  uint64_t index = 0;
  PmNsmFrameData* frame_data =
      GetFrameDataStart(client, index, window_size_in_ms);
  if (frame_data == nullptr) {
    *gpu_data = metric_cache->cached_gpu_data_[0];
    return PM_STATUS::PM_STATUS_SUCCESS;
  }

  // Calculate the end qpc based on the current frame's qpc and
  // requested window size coverted to a qpc
  uint64_t end_qpc =
      frame_data->present_event.PresentStartTime -
      ContvertMsToQpcTicks(window_size_in_ms, qpc_frequency.QuadPart);

  // Get the gpu telemetry cap bits
  std::bitset<static_cast<size_t>(GpuTelemetryCapBits::gpu_telemetry_count)>
      gpu_telemetry_cap_bits{};
  auto result = client->GetGpuTelemetryCaps();
  if (result.has_value()) {
    gpu_telemetry_cap_bits = result.value();
  }
  
  // Need special handling of gpu mem utilization as it is derived from
  // gpu mem size and gpu mem used. If those aren't present we can't
  // calculate the utilization
  bool gpu_mem_utilization_valid = gpu_telemetry_cap_bits[static_cast<size_t>(
                                       GpuTelemetryCapBits::gpu_mem_size)] &&
                                   gpu_telemetry_cap_bits[static_cast<size_t>(
                                       GpuTelemetryCapBits::gpu_mem_used)];

  uint32_t num_processed_frames = 0;

  // Loop from the most recent frame data until we either run out of data or we
  // meet the window size requirements sent in by the client
  while (frame_data->present_event.PresentStartTime > end_qpc) {
    for (size_t i = 0; i < gpu_telemetry_cap_bits.size(); i++) {
      if (gpu_telemetry_cap_bits[i]) {
        try {
          CopyPowerTelemetryItem(gpu_telemetry_items[i], psu_voltage, gpu_data,
                                 frame_data->power_telemetry, i);
        } catch (...) {}
      }
    }
    if (gpu_mem_utilization_valid) {
      gpu_mem_utilization.push_back(
          100. *
          (static_cast<double>(frame_data->power_telemetry.gpu_mem_used_b) /
           frame_data->power_telemetry.gpu_mem_total_size_b));
    }
    num_processed_frames++;

    // Get the index of the next frame
    if (DecrementIndex(nsm_view, index) == false) {
      // We have run out of data to process, time to go
      break;
    }
    frame_data = client->ReadFrameByIdx(index);
    if (frame_data == nullptr) {
      break;
    }
  }

  for (size_t i = 0; i < gpu_telemetry_cap_bits.size(); i++) {
    try {
      SetGpuData(gpu_telemetry_items[i], psu_voltage, gpu_data, i,
                 gpu_telemetry_cap_bits[i]);
    } catch (...) {}
  }

  CalculateMetricDoubleData(gpu_mem_utilization, gpu_data->gpu_mem_utilization,
                            gpu_mem_utilization_valid);

  // If no frames were processed, use cached data
  if (num_processed_frames == 0) {
    *gpu_data = metric_cache->cached_gpu_data_[0];
  } else {
    metric_cache->cached_gpu_data_[0] = *gpu_data;
  }

  return PM_STATUS::PM_STATUS_SUCCESS;

}

PM_STATUS PresentMonClient::GetCpuData(uint32_t process_id,
                                       PM_CPU_DATA* cpu_data,
                                       double window_size_in_ms) {
  std::vector<double> cpu_telemetry_items[static_cast<size_t>(
      CpuTelemetryCapBits::cpu_telemetry_count)];

  auto iter = clients_.find(process_id);

  if (iter == clients_.end()) {
    LOG(INFO) << "Stream client for process " << process_id
              << " doesn't exist. Please call pmStartStream to initialize the "
                 "client.";
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }

  StreamClient* client = iter->second.get();
  auto nsm_view = client->GetNamedSharedMemView();
  LARGE_INTEGER qpc_frequency = client->GetQpcFrequency();

  auto cache_iter = client_metric_caches_.find(process_id);
  if (cache_iter == client_metric_caches_.end()) {
    // This should never happen!
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }
  auto metric_cache = &cache_iter->second;

  uint64_t index = 0;
  PmNsmFrameData* frame_data =
      GetFrameDataStart(client, index, window_size_in_ms);
  if (frame_data == nullptr) {
    *cpu_data = metric_cache->cached_cpu_data_[0];
    return PM_STATUS::PM_STATUS_SUCCESS;
  }

  // Calculate the end qpc based on the current frame's qpc and
  // requested window size coverted to a qpc
  uint64_t end_qpc =
      frame_data->present_event.PresentStartTime -
      ContvertMsToQpcTicks(window_size_in_ms, qpc_frequency.QuadPart);

  // Get the cpu telemetry cap bits
  std::bitset<static_cast<size_t>(CpuTelemetryCapBits::cpu_telemetry_count)>
      cpu_telemetry_cap_bits{};
  auto result = client->GetCpuTelemetryCaps();
  if (result.has_value()) {
    cpu_telemetry_cap_bits = result.value();
  }

  uint32_t num_processed_frames = 0;
  // Loop from the most recent frame data until we either run out of data or we
  // meet the window size requirements sent in by the client
  while (frame_data->present_event.PresentStartTime > end_qpc) {
    for (size_t i = 0; i < cpu_telemetry_cap_bits.size(); i++) {
      if (cpu_telemetry_cap_bits[i]) {
        try {
          CopyCpuTelemetryItem(cpu_telemetry_items[i], frame_data->cpu_telemetry, i);
        } catch (...) {
        }
      }
    }
    num_processed_frames++;
    // Get the index of the next frame
    if (DecrementIndex(nsm_view, index) == false) {
      // We have run out of data to process, time to go
      break;
    }
    frame_data = client->ReadFrameByIdx(index);
    if (frame_data == nullptr) {
      break;
    }
  }

  for (size_t i = 0; i < cpu_telemetry_cap_bits.size(); i++) {
    try {
      SetCpuData(cpu_telemetry_items[i], cpu_data, i,
                 cpu_telemetry_cap_bits[i]);
    } catch (...) {
    }
  }

  // If no frames were processed, use cache data
  if (num_processed_frames == 0) {
    *cpu_data = metric_cache->cached_cpu_data_[0];
  } else {
    metric_cache->cached_cpu_data_[0] = *cpu_data;
  }

  return PM_STATUS::PM_STATUS_SUCCESS;
}

void PresentMonClient::CalculateMetricDoubleData(
    std::vector<double>& in_data, PM_METRIC_DOUBLE_DATA& metric_double_data,
    bool valid) {
  CalculateMetricDoubleDataNoAvg(in_data, metric_double_data, valid);

  if (in_data.size() > 0) {
    for (auto& element : in_data) {
      metric_double_data.avg += element;
    }
    metric_double_data.avg /= in_data.size();
  }
}

void PresentMonClient::CalculateMetricDoubleDataNoAvg(
    std::vector<double>& in_data, PM_METRIC_DOUBLE_DATA& metric_double_data,
    bool valid) {
  metric_double_data.avg = 0.0;
  if (in_data.size() > 1) {
    size_t middle_index = in_data.size() / 2;
    metric_double_data.raw = in_data[middle_index];
    std::sort(in_data.begin(), in_data.end());
    metric_double_data.low = in_data[0];
    metric_double_data.high = in_data[in_data.size() - 1];
    metric_double_data.percentile_99 = GetPercentile(in_data, 0.01);
    metric_double_data.percentile_95 = GetPercentile(in_data, 0.05);
    metric_double_data.percentile_90 = GetPercentile(in_data, 0.1);
  } else if (in_data.size() == 1) {
    metric_double_data.raw = in_data[0];
    metric_double_data.low = in_data[0];
    metric_double_data.high = in_data[0];
    metric_double_data.percentile_99 = in_data[0];
    metric_double_data.percentile_95 = in_data[0];
    metric_double_data.percentile_90 = in_data[0];
  } else {
    metric_double_data.raw = 0.;
    metric_double_data.low = 0.;
    metric_double_data.high = 0.;
    metric_double_data.percentile_99 = 0.;
    metric_double_data.percentile_95 = 0.;
    metric_double_data.percentile_90 = 0.;
  }
  metric_double_data.valid = valid;
}

// in_out_num_frames: input value indicates number of frames the out_buf can hold.
// out value indicate numbers of FrameData returned. 
PM_STATUS PresentMonClient::GetFrameData(uint32_t process_id,
                                         bool is_etl,
                                         uint32_t* in_out_num_frames,
                                         PM_FRAME_DATA* out_buf) {
  PM_STATUS status = PM_STATUS::PM_STATUS_SUCCESS;

  if (in_out_num_frames == nullptr) {
    return PM_STATUS::PM_STATUS_FAILURE;
  }

  uint32_t frames_to_copy = *in_out_num_frames;
  // We have saved off the number of frames to copy, now set
  // to zero in case we error out along the way BEFORE we
  // copy frames into the buffer. If a successful copy occurs
  // we'll set to actual number copied.
  *in_out_num_frames = 0;
  uint32_t frames_copied = 0;
  *in_out_num_frames = 0;

  StreamClient* client = nullptr;
  if (is_etl) {
    if (etl_client_) {
      client = etl_client_.get();
    } else {
      LOG(INFO)
          << "Stream client for process "
          << " doesn't exist. Please call pmStartStream to initialize the "
             "client.";
      return PM_STATUS::PM_STATUS_INVALID_PID;
    }
  } else {
    auto iter = clients_.find(process_id);

    if (iter == clients_.end()) {
      try {
        LOG(INFO)
            << "Stream client for process " << process_id
            << " doesn't exist. Please call pmStartStream to initialize the "
               "client.";
      } catch (...) {
        LOG(INFO)
            << "Stream client for process "
            << " doesn't exist. Please call pmStartStream to initialize the "
               "client.";
      }
      return PM_STATUS::PM_STATUS_INVALID_PID;
    }

    client = iter->second.get();
  }

  auto nsm_view = client->GetNamedSharedMemView();
  auto nsm_hdr = nsm_view->GetHeader();
  if (!nsm_hdr->process_active) {
    // Service destroyed the named shared memory due to process exit. Destroy
    // the mapped view from client side.
    if (is_etl == false) {
      StopStreamProcess(process_id);
    }
    return PM_STATUS::PM_STATUS_INVALID_PID;
  }

  uint64_t last_frame_idx = client->GetLatestFrameIndex();
  if (last_frame_idx == UINT_MAX) {
    // There are no frames available
    return PM_STATUS::PM_STATUS_NO_DATA;
  }

  PM_FRAME_DATA* dst_frame = out_buf;

  for (uint32_t i = 0; i < frames_to_copy; i++) {
    if (is_etl) {
      status = client->DequeueFrame(&dst_frame);
    } else {
      status = client->RecordFrame(&dst_frame);
    }
    if (status != PM_STATUS::PM_STATUS_SUCCESS) {
      break;
    }

    dst_frame++;
    frames_copied++;
  }

  if ((status == PM_STATUS::PM_STATUS_NO_DATA) && (frames_copied > 0)) {
    // There are no more frames available in the NSM but frames have been
    // processed. Update status to PM_SUCCESS
    status = PM_STATUS::PM_STATUS_SUCCESS;
  }
  // Set to the actual number of frames copied
  *in_out_num_frames = frames_copied;

  return status;
}

PM_STATUS PresentMonClient::SetMetricsOffset(double offset_in_ms) {
  //(TODO)megalvan: think about removing storing qpc frequency from the
  // stream client as we should not need the process id for setting the
  // metric offset
  LARGE_INTEGER qpc_frequency;
  QueryPerformanceFrequency(&qpc_frequency);
  set_metric_offset_in_qpc_ticks_ =
      ContvertMsToQpcTicks(offset_in_ms, qpc_frequency.QuadPart);
  return PM_STATUS::PM_STATUS_SUCCESS;
}

uint64_t PresentMonClient::GetAdjustedQpc(uint64_t current_qpc,
                                          uint64_t frame_data_qpc,
                                          LARGE_INTEGER frequency) {
  // Calculate how far behind the frame data qpc is compared
  // to the client qpc
  uint64_t current_qpc_delta = current_qpc - frame_data_qpc;
  if (client_to_frame_data_delta_ == 0) {
    client_to_frame_data_delta_ = current_qpc_delta;
  } else {
    if (_abs64(client_to_frame_data_delta_ - current_qpc_delta) >
        kClientFrameDeltaQPCThreshold) {
      client_to_frame_data_delta_ = current_qpc_delta;
    }
  }

  // Add in the client set metric offset in qpc ticks
  return current_qpc -
         (client_to_frame_data_delta_ + set_metric_offset_in_qpc_ticks_);
}

PmNsmFrameData* PresentMonClient::GetFrameDataStart(
    StreamClient* client, uint64_t& index, double& window_sample_size_in_ms) {

  PmNsmFrameData* frame_data = nullptr;
  index = 0;
  if (client == nullptr) {
    return nullptr;
  }

  auto nsm_view = client->GetNamedSharedMemView();
  auto nsm_hdr = nsm_view->GetHeader();
  if (!nsm_hdr->process_active) {
    return nullptr;
  }

  index = client->GetLatestFrameIndex();
  frame_data = client->ReadFrameByIdx(index);
  if (frame_data == nullptr) {
    index = 0;
    return nullptr;
  }
  
  if (set_metric_offset_in_qpc_ticks_ == 0) {
    // Client has not specified a metric offset. Return back the most
    // most recent frame data
    return frame_data;
  }

  LARGE_INTEGER client_qpc = {};
  QueryPerformanceCounter(&client_qpc);
  uint64_t adjusted_qpc = GetAdjustedQpc(
      client_qpc.QuadPart, frame_data->present_event.PresentStartTime,
                     client->GetQpcFrequency());

  if (adjusted_qpc > frame_data->present_event.PresentStartTime) {
    // Need to adjust the size of the window sample size
    double ms_adjustment =
        QpcDeltaToMs(adjusted_qpc - frame_data->present_event.PresentStartTime,
                     client->GetQpcFrequency());
    window_sample_size_in_ms = window_sample_size_in_ms - ms_adjustment;
    if (window_sample_size_in_ms <= 0.0) {
      return nullptr;
    }
  } else {
    // Find the frame with the appropriate time based on the adjusted
    // qpc
    for (;;) {
      
      if (DecrementIndex(nsm_view, index) == false) {
        // Increment index to match up with the frame_data read below
        index++;
        break;
      }
      frame_data = client->ReadFrameByIdx(index);
      if (frame_data == nullptr) {
        return nullptr;
      }
      if (adjusted_qpc >= frame_data->present_event.PresentStartTime) {
        break;
      }
    }
  }

  return frame_data;
}

bool PresentMonClient::DecrementIndex(NamedSharedMem* nsm_view,
    uint64_t& index) {

  if (nsm_view == nullptr) {
    return false;
  }

  auto nsm_hdr = nsm_view->GetHeader();
  if (!nsm_hdr->process_active) {
    return false;
  }

  uint64_t current_max_entries =
      (nsm_view->IsFull()) ? nsm_hdr->max_entries - 1 : nsm_hdr->tail_idx;
  index = (index == 0) ? current_max_entries : index - 1;
  if (index == nsm_hdr->head_idx) {
    return false;
  }

  return true;
}

uint64_t PresentMonClient::ContvertMsToQpcTicks(double time_in_ms,
                                                uint64_t frequency) {
  return static_cast<uint64_t>((time_in_ms / 1000.0) * frequency);
}

PM_STATUS PresentMonClient::EnumerateAdapters(
    PM_ADAPTER_INFO* adapter_info_buffer, uint32_t* adapter_count) {
  MemBuffer rqst_buf;
  MemBuffer rsp_buf;
  
  NamedPipeHelper::EncodeRequestHeader(&rqst_buf,PM_ACTION::ENUMERATE_ADAPTERS);

  PM_STATUS status = CallPmService(&rqst_buf, &rsp_buf);
  if (status != PM_STATUS::PM_STATUS_SUCCESS) {
    return status;
  }
  
  IPMAdapterInfo adapter_info{};
  status =
      NamedPipeHelper::DecodeEnumerateAdaptersResponse(&rsp_buf, &adapter_info);
  if (status != PM_STATUS::PM_STATUS_SUCCESS) {
    return status;
  }

  if (!adapter_info_buffer) {
    *adapter_count = adapter_info.num_adapters;
    // if buffer size too small, signal required size with error
  } else if (*adapter_count < adapter_info.num_adapters) {
    *adapter_count = adapter_info.num_adapters;
    return PM_STATUS::PM_STATUS_INSUFFICIENT_BUFFER;
    // buffer exists and is large enough, fill it and signal number of entries
    // filled
  } else {
    *adapter_count = adapter_info.num_adapters;
    std::ranges::copy(
        std::span{adapter_info.adapters, adapter_info.num_adapters},
        adapter_info_buffer);
  }
  return PM_STATUS::PM_STATUS_SUCCESS;
}

PM_STATUS PresentMonClient::SetActiveAdapter(uint32_t adapter_id) {
  MemBuffer rqst_buf;
  MemBuffer rsp_buf;
  
  NamedPipeHelper::EncodeGeneralSetActionRequest(PM_ACTION::SELECT_ADAPTER,
                                                 &rqst_buf, adapter_id);
  
  PM_STATUS status = CallPmService(&rqst_buf, &rsp_buf);
  if (status != PM_STATUS::PM_STATUS_SUCCESS) {
    return status;
  }

  status = NamedPipeHelper::DecodeGeneralSetActionResponse(
      PM_ACTION::SELECT_ADAPTER, &rsp_buf);

  return status;
}

PM_STATUS PresentMonClient::GetCpuName(char* cpu_name_buffer,
                                       uint32_t* buffer_size) {
  MemBuffer rqst_buf;
  MemBuffer rsp_buf;

  // buffer_size must be valid to return back size of size
  // of cpu name
  if (buffer_size == nullptr) {
    return PM_STATUS::PM_STATUS_INSUFFICIENT_BUFFER;
  }

  NamedPipeHelper::EncodeRequestHeader(&rqst_buf, PM_ACTION::GET_STATIC_CPU_METRICS);

  PM_STATUS status = CallPmService(&rqst_buf, &rsp_buf);
  if (status != PM_STATUS::PM_STATUS_SUCCESS) {
    return status;
  }

  IPMStaticCpuMetrics static_cpu_metrics{};
  status = NamedPipeHelper::DecodeStaticCpuMetricsResponse(&rsp_buf, &static_cpu_metrics);
  if (status != PM_STATUS::PM_STATUS_SUCCESS ||
      static_cpu_metrics.cpuNameLength > MAX_PM_CPU_NAME) {
    return status;
  }

  if (cpu_name_buffer == nullptr) {
    // Service returns back the cpu name length without a
    // NULL terminator. Increase by one as client present mon client
    // will return a size including null terminator
    *buffer_size = static_cpu_metrics.cpuNameLength + 1;
    return PM_STATUS::PM_STATUS_INSUFFICIENT_BUFFER;
  } else {
    std::string temp_string = static_cpu_metrics.cpuName;
    try {
      temp_string.copy(cpu_name_buffer, (size_t)*buffer_size - 1, 0);
      // Add a null terminator manually
      cpu_name_buffer[static_cpu_metrics.cpuNameLength] = '\0';
      // Update the buffer size to the size of the cpu name +
      // the null terminator
      *buffer_size = static_cpu_metrics.cpuNameLength + 1;
      if (static_cpu_metrics.cpuNameLength + 1 > *buffer_size) {
        return PM_STATUS::PM_STATUS_INSUFFICIENT_BUFFER;
      } else {
        return PM_STATUS::PM_STATUS_SUCCESS;
      }
    } catch (...) {
      return PM_STATUS::PM_STATUS_FAILURE;
    }
  }
}

PM_STATUS PresentMonClient::SetGPUTelemetryPeriod(uint32_t period_ms) {
  MemBuffer rqst_buf;
  MemBuffer rsp_buf;

  NamedPipeHelper::EncodeGeneralSetActionRequest(
      PM_ACTION::SET_GPU_TELEMETRY_PERIOD, &rqst_buf, period_ms);

  PM_STATUS status = CallPmService(&rqst_buf, &rsp_buf);
  if (status != PM_STATUS::PM_STATUS_SUCCESS) {
    return status;
  }

  status = NamedPipeHelper::DecodeGeneralSetActionResponse(
      PM_ACTION::SET_GPU_TELEMETRY_PERIOD, &rsp_buf);
  return status;
}

void PresentMonClient::CopyPowerTelemetryItem(
    std::vector<double>& telemetry_item, std::vector<double> psu_voltage[],
    PM_GPU_DATA* gpu_data,
    PresentMonPowerTelemetryInfo& power_telemetry_info,
    size_t telemetry_item_bit) {
  GpuTelemetryCapBits bit =
      static_cast<GpuTelemetryCapBits>(telemetry_item_bit);
  switch (bit) {
    case GpuTelemetryCapBits::time_stamp:
      // This is a valid telemetry cap bit but we do not produce metrics for
      // it. Silently ignore...
      break;
    case GpuTelemetryCapBits::gpu_power:
      telemetry_item.push_back(power_telemetry_info.gpu_power_w);
      break;
    case GpuTelemetryCapBits::gpu_sustained_power_limit:
      telemetry_item.push_back(
          power_telemetry_info.gpu_sustained_power_limit_w);
      break;
    case GpuTelemetryCapBits::gpu_voltage:
      telemetry_item.push_back(power_telemetry_info.gpu_voltage_v);
      break;
    case GpuTelemetryCapBits::gpu_frequency:
      telemetry_item.push_back(power_telemetry_info.gpu_frequency_mhz);
      break;
    case GpuTelemetryCapBits::gpu_temperature:
      telemetry_item.push_back(power_telemetry_info.gpu_temperature_c);
      break;
    case GpuTelemetryCapBits::gpu_utilization:
      telemetry_item.push_back(power_telemetry_info.gpu_utilization);
      break;
    case GpuTelemetryCapBits::gpu_render_compute_utilization:
      telemetry_item.push_back(
          power_telemetry_info.gpu_render_compute_utilization);
      break;
    case GpuTelemetryCapBits::gpu_media_utilization:
      telemetry_item.push_back(power_telemetry_info.gpu_media_utilization);
      break;
    case GpuTelemetryCapBits::vram_power:
      telemetry_item.push_back(power_telemetry_info.gpu_media_utilization);
      break;
    case GpuTelemetryCapBits::vram_voltage:
      telemetry_item.push_back(power_telemetry_info.vram_voltage_v);
      break;
    case GpuTelemetryCapBits::vram_frequency:
      telemetry_item.push_back(power_telemetry_info.vram_frequency_mhz);
      break;
    case GpuTelemetryCapBits::vram_effective_frequency:
      telemetry_item.push_back(
          power_telemetry_info.vram_effective_frequency_gbps);
      break;
    case GpuTelemetryCapBits::vram_temperature:
      telemetry_item.push_back(power_telemetry_info.vram_temperature_c);
      break;
    case GpuTelemetryCapBits::fan_speed_0:
      telemetry_item.push_back(power_telemetry_info.fan_speed_rpm[0]);
      break;
    case GpuTelemetryCapBits::fan_speed_1:
      telemetry_item.push_back(power_telemetry_info.fan_speed_rpm[1]);
      break;
    case GpuTelemetryCapBits::fan_speed_2:
      telemetry_item.push_back(power_telemetry_info.fan_speed_rpm[2]);
      break;
    case GpuTelemetryCapBits::fan_speed_3:
      telemetry_item.push_back(power_telemetry_info.fan_speed_rpm[3]);
      break;
    case GpuTelemetryCapBits::fan_speed_4:
      telemetry_item.push_back(power_telemetry_info.fan_speed_rpm[4]);
      break;
    case GpuTelemetryCapBits::psu_info_0:
      telemetry_item.push_back(power_telemetry_info.psu[0].psu_power);
      psu_voltage[0].push_back(power_telemetry_info.psu[0].psu_voltage);
      gpu_data->psu_data[0].psu_type =
          TranslatePsuType(power_telemetry_info.psu[0].psu_type);
      gpu_data->psu_data[0].valid = true;
      break;
    case GpuTelemetryCapBits::psu_info_1:
      telemetry_item.push_back(power_telemetry_info.psu[1].psu_power);
      psu_voltage[1].push_back(power_telemetry_info.psu[1].psu_voltage);
      gpu_data->psu_data[1].psu_type =
          TranslatePsuType(power_telemetry_info.psu[1].psu_type);
      gpu_data->psu_data[1].valid = true;
      break;
    case GpuTelemetryCapBits::psu_info_2:
      telemetry_item.push_back(power_telemetry_info.psu[2].psu_power);
      psu_voltage[2].push_back(power_telemetry_info.psu[2].psu_voltage);
      gpu_data->psu_data[2].psu_type =
          TranslatePsuType(power_telemetry_info.psu[2].psu_type);
      gpu_data->psu_data[2].valid = true;
      break;
    case GpuTelemetryCapBits::psu_info_3:
      telemetry_item.push_back(power_telemetry_info.psu[3].psu_power);
      psu_voltage[3].push_back(power_telemetry_info.psu[3].psu_voltage);
      gpu_data->psu_data[3].psu_type =
          TranslatePsuType(power_telemetry_info.psu[3].psu_type);
      gpu_data->psu_data[3].valid = true;
      break;
    case GpuTelemetryCapBits::psu_info_4:
      telemetry_item.push_back(power_telemetry_info.psu[4].psu_power);
      psu_voltage[4].push_back(power_telemetry_info.psu[4].psu_voltage);
      gpu_data->psu_data[4].psu_type =
          TranslatePsuType(power_telemetry_info.psu[4].psu_type);
      gpu_data->psu_data[4].valid = true;
      break;
    case GpuTelemetryCapBits::gpu_mem_size:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.gpu_mem_total_size_b));
      break;
    case GpuTelemetryCapBits::gpu_mem_used:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.gpu_mem_used_b));
      break;
    case GpuTelemetryCapBits::gpu_mem_max_bandwidth:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.gpu_mem_max_bandwidth_bps));
      break;
    case GpuTelemetryCapBits::gpu_mem_write_bandwidth:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.gpu_mem_write_bandwidth_bps));
      break;
    case GpuTelemetryCapBits::gpu_mem_read_bandwidth:
      telemetry_item.push_back(
          power_telemetry_info.gpu_mem_read_bandwidth_bps);
      break;
    case GpuTelemetryCapBits::gpu_power_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.gpu_power_limited));
      break;
    case GpuTelemetryCapBits::gpu_temperature_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.gpu_temperature_limited));
      break;
    case GpuTelemetryCapBits::gpu_current_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.gpu_current_limited));
      break;
    case GpuTelemetryCapBits::gpu_voltage_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.gpu_voltage_limited));
      break;
    case GpuTelemetryCapBits::gpu_utilization_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.gpu_utilization_limited));
      break;
    case GpuTelemetryCapBits::vram_power_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.vram_power_limited));
      break;
    case GpuTelemetryCapBits::vram_temperature_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.vram_temperature_limited));
      break;
    case GpuTelemetryCapBits::vram_current_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.vram_current_limited));
      break;
    case GpuTelemetryCapBits::vram_voltage_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.vram_voltage_limited));
      break;
    case GpuTelemetryCapBits::vram_utilization_limited:
      telemetry_item.push_back(
          static_cast<double>(power_telemetry_info.vram_utilization_limited));
      break;
    default:
      throw std::runtime_error{"Unknown Telemetry Bit"};
      break;
  }
  return;
}

void PresentMonClient::CopyCpuTelemetryItem(
    std::vector<double>& telemetry_item, CpuTelemetryInfo& cpu_telemetry_info,
    size_t telemetry_item_bit) {
  CpuTelemetryCapBits bit =
      static_cast<CpuTelemetryCapBits>(telemetry_item_bit);
  switch (bit) { 
    case CpuTelemetryCapBits::cpu_utilization:
      telemetry_item.push_back(cpu_telemetry_info.cpu_utilization);
      break;
    case CpuTelemetryCapBits::cpu_power:
      telemetry_item.push_back(cpu_telemetry_info.cpu_power_w);
      break;
    case CpuTelemetryCapBits::cpu_power_limit:
      telemetry_item.push_back(cpu_telemetry_info.cpu_power_limit_w);
      break;
    case CpuTelemetryCapBits::cpu_temperature:
      telemetry_item.push_back(cpu_telemetry_info.cpu_temperature);
      break;
    case CpuTelemetryCapBits::cpu_frequency:
      telemetry_item.push_back(cpu_telemetry_info.cpu_frequency);
      break;
    default:
      throw std::runtime_error{"Unknown Telemetry Bit"};
  }
}

void PresentMonClient::SetGpuData(std::vector<double>& telemetry_item,
                                  std::vector<double> psu_voltage[],
                                  PM_GPU_DATA* gpu_data,
                                  size_t telemetry_item_bit, bool valid) {
  GpuTelemetryCapBits bit =
      static_cast<GpuTelemetryCapBits>(telemetry_item_bit);
  switch (bit) {
    case GpuTelemetryCapBits::time_stamp:
      // This is a valid telemetry cap bit but we do not produce metrics for
      // it. Silently ignore...
      break;
    case GpuTelemetryCapBits::gpu_power:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_power_w, valid);
      break;
    case GpuTelemetryCapBits::gpu_sustained_power_limit:
      CalculateMetricDoubleData(telemetry_item,
                                gpu_data->gpu_sustained_power_limit_w, valid);
      break;
    case GpuTelemetryCapBits::gpu_voltage:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_voltage_v, valid);
      break;
    case GpuTelemetryCapBits::gpu_frequency:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_frequency_mhz,
                                valid);
      break;
    case GpuTelemetryCapBits::gpu_temperature:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_temperature_c,
                                valid);
      break;
    case GpuTelemetryCapBits::gpu_utilization:
      CalculateMetricDoubleData(
          telemetry_item, gpu_data->gpu_utilization, valid);
      break;
    case GpuTelemetryCapBits::gpu_render_compute_utilization:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_render_compute_utilization,
                                valid);
      break;
    case GpuTelemetryCapBits::gpu_media_utilization:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_media_utilization,
                                valid);
      break;
    case GpuTelemetryCapBits::vram_power:
      CalculateMetricDoubleData(telemetry_item, gpu_data->vram_power_w, valid);
      break;
    case GpuTelemetryCapBits::vram_voltage:
      CalculateMetricDoubleData(telemetry_item, gpu_data->vram_voltage_v,
                                valid);
      break;
    case GpuTelemetryCapBits::vram_frequency:
      CalculateMetricDoubleData(telemetry_item, gpu_data->vram_frequency_mhz,
                                valid);
      break;
    case GpuTelemetryCapBits::vram_effective_frequency:
      CalculateMetricDoubleData(telemetry_item,
                                gpu_data->vram_effective_frequency_gbps, valid);
      break;
    case GpuTelemetryCapBits::vram_temperature:
      CalculateMetricDoubleData(telemetry_item, gpu_data->vram_temperature_c,
                                valid);
      break;
    case GpuTelemetryCapBits::fan_speed_0:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_fan_speed_rpm[0],
                                valid);
      break;
    case GpuTelemetryCapBits::fan_speed_1:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_fan_speed_rpm[1],
                                valid);
      break;
    case GpuTelemetryCapBits::fan_speed_2:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_fan_speed_rpm[2],
                                valid);
      break;
    case GpuTelemetryCapBits::fan_speed_3:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_fan_speed_rpm[3],
                                valid);
      break;
    case GpuTelemetryCapBits::fan_speed_4:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_fan_speed_rpm[4],
                                valid);
      break;
    case GpuTelemetryCapBits::psu_info_0:
      CalculateMetricDoubleData(telemetry_item, gpu_data->psu_data[0].psu_power,
                                valid);
      CalculateMetricDoubleData(psu_voltage[0],
                                gpu_data->psu_data[0].psu_voltage, valid);
      break;
    case GpuTelemetryCapBits::psu_info_1:
      CalculateMetricDoubleData(telemetry_item, gpu_data->psu_data[1].psu_power,
                                valid);
      CalculateMetricDoubleData(psu_voltage[1],
                                gpu_data->psu_data[1].psu_voltage, valid);
      break;
    case GpuTelemetryCapBits::psu_info_2:
      CalculateMetricDoubleData(telemetry_item, gpu_data->psu_data[2].psu_power,
                                valid);
      CalculateMetricDoubleData(psu_voltage[2],
                                gpu_data->psu_data[2].psu_voltage, valid);
      break;
    case GpuTelemetryCapBits::psu_info_3:
      CalculateMetricDoubleData(telemetry_item, gpu_data->psu_data[3].psu_power,
                                valid);
      CalculateMetricDoubleData(psu_voltage[3],
                                gpu_data->psu_data[3].psu_voltage, valid);
      break;
    case GpuTelemetryCapBits::psu_info_4:
      CalculateMetricDoubleData(telemetry_item, gpu_data->psu_data[4].psu_power,
                                valid);
      CalculateMetricDoubleData(psu_voltage[4],
                                gpu_data->psu_data[4].psu_voltage, valid);
      break;
    case GpuTelemetryCapBits::gpu_mem_size:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_mem_total_size_b,
                                valid);
      break;
    case GpuTelemetryCapBits::gpu_mem_used:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_mem_used_b,
                                valid);
      break;
    case GpuTelemetryCapBits::gpu_mem_max_bandwidth:
      CalculateMetricDoubleData(telemetry_item,
                                gpu_data->gpu_mem_max_bandwidth_bps, valid);
      break;
    case GpuTelemetryCapBits::gpu_mem_write_bandwidth:
      CalculateMetricDoubleData(telemetry_item,
                                gpu_data->gpu_mem_write_bandwidth_bps, valid);
      break;
    case GpuTelemetryCapBits::gpu_mem_read_bandwidth:
      CalculateMetricDoubleData(telemetry_item,
                                gpu_data->gpu_mem_read_bandwidth_bps, valid);
      break;
    case GpuTelemetryCapBits::gpu_power_limited:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_power_limited,
                                valid);
      break;
    case GpuTelemetryCapBits::gpu_temperature_limited:
      CalculateMetricDoubleData(telemetry_item,
                                gpu_data->gpu_temperature_limited, valid);
      break;
    case GpuTelemetryCapBits::gpu_current_limited:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_current_limited,
                                valid);
      break;
    case GpuTelemetryCapBits::gpu_voltage_limited:
      CalculateMetricDoubleData(telemetry_item, gpu_data->gpu_voltage_limited,
                                valid);
      break;
    case GpuTelemetryCapBits::gpu_utilization_limited:
      CalculateMetricDoubleData(telemetry_item,
                                gpu_data->gpu_utilization_limited, valid);
      break;
    case GpuTelemetryCapBits::vram_power_limited:
      CalculateMetricDoubleData(telemetry_item, gpu_data->vram_power_limited,
                                valid);
      break;
    case GpuTelemetryCapBits::vram_temperature_limited:
      CalculateMetricDoubleData(telemetry_item,
                                gpu_data->vram_temperature_limited, valid);
      break;
    case GpuTelemetryCapBits::vram_current_limited:
      CalculateMetricDoubleData(telemetry_item, gpu_data->vram_current_limited,
                                valid);
      break;
    case GpuTelemetryCapBits::vram_voltage_limited:
      CalculateMetricDoubleData(telemetry_item, gpu_data->vram_voltage_limited,
                                valid);
      break;
    case GpuTelemetryCapBits::vram_utilization_limited:
      CalculateMetricDoubleData(telemetry_item,
                                gpu_data->vram_utilization_limited, valid);
      break;
    default:
      throw std::runtime_error{"Unknown Telemetry Bit"};
      break;
  }
}

void PresentMonClient::SetCpuData(std::vector<double>& telemetry_item,
                                  PM_CPU_DATA* cpu_data,
                                  size_t telemetry_item_bit, bool valid) {
  CpuTelemetryCapBits bit =
      static_cast<CpuTelemetryCapBits>(telemetry_item_bit);
  switch (bit) {
    case CpuTelemetryCapBits::cpu_utilization:
      CalculateMetricDoubleData(telemetry_item,
                                cpu_data->cpu_utilization, valid);
      break;
    case CpuTelemetryCapBits::cpu_power:
      CalculateMetricDoubleData(telemetry_item, cpu_data->cpu_power_w,
                                valid);
      break;
    case CpuTelemetryCapBits::cpu_power_limit:
      CalculateMetricDoubleData(telemetry_item, cpu_data->cpu_power_limit_w,
                                valid);
      break;
    case CpuTelemetryCapBits::cpu_temperature:
      CalculateMetricDoubleData(telemetry_item, cpu_data->cpu_temperature_c,
                                valid);
      break;
    case CpuTelemetryCapBits::cpu_frequency:
      CalculateMetricDoubleData(telemetry_item, cpu_data->cpu_frequency,
                                valid);
      break;
    default:
      throw std::runtime_error{"Unknown Telemetry Bit"};
      break;
  }
}

void PresentMonClient::InitializeClientLogging() {
  if (!google::IsGoogleLoggingInitialized()) {
    char localAppDataPath[MAX_PATH];
    HRESULT result =
        SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppDataPath);

    if (SUCCEEDED(result)) {
      std::string pm_app_local_path(localAppDataPath);
      pm_app_local_path += "\\Intel\\PresentMon\\logs";
      std::filesystem::create_directories(pm_app_local_path);
      InitializeLogging(pm_app_local_path.c_str(), "pm-sdk", ".log", 0);
    }
  }
}