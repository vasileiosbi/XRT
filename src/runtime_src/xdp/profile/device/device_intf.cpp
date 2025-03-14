/**
 * Copyright (C) 2016-2019 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include "device_intf.h"

#include "xclperf.h"
#include "xcl_perfmon_parameters.h"
#include "tracedefs.h"

#include <iostream>
#include <cstdio>
#include <cstring>
//#include <algorithm>
#include <thread>
#include <vector>
//#include <time.h>
#include <string.h>
#include <chrono>

#ifndef _WINDOWS
// TODO: Windows build support
//    unistd.h is linux only header file
//    it is included for read, write, close, lseek64
#include <unistd.h>
#endif

#ifdef _WINDOWS
#define __func__ __FUNCTION__
#endif

namespace xdp {

DeviceIntf::~DeviceIntf()
{
    for(auto mon : aimList) {
        delete mon;
    }
    for(auto mon : amList) {
        delete mon;
    }
    for(auto mon : asmList) {
        delete mon;
    }
    aimList.clear();
    amList.clear();
    asmList.clear();

    delete fifoCtrl;
    delete fifoRead;
    delete traceFunnel;
    delete traceDMA;

    delete mDevice;
}

  // ***************************************************************************
  // Read/Write
  // ***************************************************************************

#if 0
  size_t DeviceIntf::write(uint64_t offset, const void *hostBuf, size_t size)
  {
    if (mDeviceHandle == nullptr)
      return 0;
	return xclWrite(mDeviceHandle, XCL_ADDR_SPACE_DEVICE_PERFMON, offset, hostBuf, size);
  }

  size_t DeviceIntf::read(uint64_t offset, void *hostBuf, size_t size)
  {
    if (mDeviceHandle == nullptr)
      return 0;
	return xclRead(mDeviceHandle, XCL_ADDR_SPACE_DEVICE_PERFMON, offset, hostBuf, size);
  }

  size_t DeviceIntf::traceRead(void *buffer, size_t size, uint64_t addr)
  {
    if (mDeviceHandle == nullptr)
      return 0;
    return xclUnmgdPread(mDeviceHandle, 0, buffer, size, addr);
  }
#endif

  void DeviceIntf::setDevice(xdp::Device* devHandle)
  {
    if(mDevice && mDevice != devHandle) {
      // ERROR : trying to set device when it is already populated with some other device
      return;
    }
    mDevice = devHandle; 
  }



  // ***************************************************************************
  // Debug IP Layout
  // ***************************************************************************
  
  uint32_t DeviceIntf::getNumMonitors(xclPerfMonType type)
  {
    if (type == XCL_PERF_MON_MEMORY)
      return aimList.size();
    if (type == XCL_PERF_MON_ACCEL)
      return amList.size();
    if (type == XCL_PERF_MON_STR)
      return asmList.size();

    if(type == XCL_PERF_MON_STALL) {
      uint32_t count = 0;
      for(auto mon : amList) {
        if(mon->hasStall())  count++;
      }
      return count;
    }

    if(type == XCL_PERF_MON_HOST) {
      uint32_t count = 0;
      for(auto mon : aimList) {
        if(mon->isHostMonitor())  count++;
      }
      return count;
    }

    // FIFO ?

    if(type == XCL_PERF_MON_SHELL) {
      uint32_t count = 0;
      for(auto mon : aimList) {
        if(mon->isShellMonitor())  count++;
      }
      return count;
    }
    return 0;
  }

  void DeviceIntf::getMonitorName(xclPerfMonType type, uint32_t index, char* name, uint32_t length)
  {
    std::string str = "";
    if((type == XCL_PERF_MON_MEMORY) && (index < aimList.size())) { str = aimList[index]->getName(); }
    if((type == XCL_PERF_MON_ACCEL)  && (index < amList.size()))  { str = amList[index]->getName(); }
    if((type == XCL_PERF_MON_STR)    && (index < asmList.size())) { str = asmList[index]->getName(); }
    strncpy(name, str.c_str(), length);
    if(str.length() >= length) name[length-1] = '\0'; // required ??
  }

  uint32_t DeviceIntf::getMonitorProperties(xclPerfMonType type, uint32_t index)
  {
    if((type == XCL_PERF_MON_MEMORY) && (index < aimList.size())) { return aimList[index]->getProperties(); }
    if((type == XCL_PERF_MON_ACCEL)  && (index < amList.size()))  { return amList[index]->getProperties(); }
    if((type == XCL_PERF_MON_STR)    && (index < asmList.size())) { return asmList[index]->getProperties(); }
    return 0;
  }

  // ***************************************************************************
  // Counters
  // ***************************************************************************

  // Start device counters performance monitoring
  size_t DeviceIntf::startCounters(xclPerfMonType type)
  {
    if (mVerbose) {
      std::cout << __func__ << ", " << std::this_thread::get_id() << ", "
                << type << ", Start device counters..." << std::endl;
    }

    // Update addresses for debug/profile IP
//    readDebugIPlayout();

    if (!mIsDeviceProfiling)
   	  return 0;

    size_t size = 0;

    // Axi Interface Mons
    for(auto mon : aimList) {
        size += mon->startCounter();
    }
    // Accelerator Mons
    for(auto mon : amList) {
        size += mon->startCounter();
    }

    // Axi Stream Mons
    for(auto mon : asmList) {
        size += mon->startCounter();
    }
    return size;
  }

  // Stop both profile and trace performance monitoring
  size_t DeviceIntf::stopCounters(xclPerfMonType type) {
    if (mVerbose) {
      std::cout << __func__ << ", " << std::this_thread::get_id() << ", "
          << type << ", Stop and reset device counters..." << std::endl;
    }

    if (!mIsDeviceProfiling)
   	  return 0;

    size_t size = 0;

    // Axi Interface Mons
    for(auto mon : aimList) {
        size += mon->stopCounter();
    }


#if 0
    // These aren't enabled in IP
    // Accelerator Mons
    for(auto mon : amList) {
        size += mon->stopCounter();
    }

    // Axi Stream Mons
    for(auto mon : asmList) {
        size += mon->stopCounter();
    }
#endif
    return size;
  }

  // Read AIM performance counters
  size_t DeviceIntf::readCounters(xclPerfMonType type, xclCounterResults& counterResults) {
    if (mVerbose) {
      std::cout << __func__ << ", " << std::this_thread::get_id()
      << ", " << type << ", " << &counterResults
      << ", Read device counters..." << std::endl;
    }

    // Initialize all values in struct to 0
    memset(&counterResults, 0, sizeof(xclCounterResults));

    if (!mIsDeviceProfiling)
   	  return 0;

    size_t size = 0;

    // Read all Axi Interface Mons
    uint32_t idx = 0;
    for(auto mon : aimList) {
        size += mon->readCounter(counterResults, idx++);
    }

    // Read all Accelerator Mons
    idx = 0;
    for(auto mon : amList) {
        size += mon->readCounter(counterResults, idx++);
    }

    // Read all Axi Stream Mons
    idx = 0;
    for(auto mon : asmList) {
        size += mon->readCounter(counterResults, idx++);
    }

    return size;
  }

  // ***************************************************************************
  // Timeline Trace
  // ***************************************************************************

  // Start trace performance monitoring
  size_t DeviceIntf::startTrace(xclPerfMonType type, uint32_t startTrigger)
  {
    // StartTrigger Bits:
    // Bit 0: Trace Coarse/Fine     Bit 1: Transfer Trace Ctrl
    // Bit 2: CU Trace Ctrl         Bit 3: INT Trace Ctrl
    // Bit 4: Str Trace Ctrl        Bit 5: Ext Trace Ctrl
    if (mVerbose) {
      std::cout << __func__ << ", " << std::this_thread::get_id()
                << ", " << type << ", " << startTrigger
                << ", Start device tracing..." << std::endl;
    }
    size_t size = 0;

    // This just writes to trace control register
    // Axi Interface Mons
    for(auto mon : aimList) {
        size += mon->triggerTrace(startTrigger);
    }
    // Accelerator Mons
    for(auto mon : amList) {
        size += mon->triggerTrace(startTrigger);
    }
    // Axi Stream Mons
    for(auto mon : asmList) {
        size += mon->triggerTrace(startTrigger);
    }

    if (fifoCtrl)
      fifoCtrl->reset();

    if (traceFunnel)
      traceFunnel->initiateClockTraining();

    return size;
  }

  // Stop trace performance monitoring
  size_t DeviceIntf::stopTrace(xclPerfMonType type)
  {
    if (mVerbose) {
      std::cout << __func__ << ", " << std::this_thread::get_id() << ", "
                << type << ", Stop and reset device tracing..." << std::endl;
    }

    if (!mIsDeviceProfiling || !fifoCtrl)
   	  return 0;

    return fifoCtrl->reset();
  }

  // Get trace word count
  uint32_t DeviceIntf::getTraceCount(xclPerfMonType type) {
    if (mVerbose) {
      std::cout << __func__ << ", " << std::this_thread::get_id()
                << ", " << type << std::endl;
    }

    if (!mIsDeviceProfiling || !fifoCtrl)
   	  return 0;

    return fifoCtrl->getNumTraceSamples();
  }

  // Read all values from APM trace AXI stream FIFOs
  size_t DeviceIntf::readTrace(xclPerfMonType type, xclTraceResultsVector& traceVector)
  {
    if (mVerbose) {
      std::cout << __func__ << ", " << std::this_thread::get_id()
                << ", " << type << ", " << &traceVector
                << ", Reading device trace stream..." << std::endl;
    }

    traceVector.mLength = 0;
    if (!mIsDeviceProfiling)
   	  return 0;

    size_t size = 0;
    size += fifoRead->readTrace(traceVector, getTraceCount(type /*does not matter*/));

    return size;
  }

  void DeviceIntf::readDebugIPlayout()
  {
    if(mIsDebugIPlayoutRead || !mDevice)
        return;

    std::string path = mDevice->getDebugIPlayoutPath();
    if(path.empty()) {
        // error ? : for HW_emu this will be empty for now ; but as of current status should not have been called 
        return;
    }

    uint32_t liveProcessesOnDevice = mDevice->getNumLiveProcesses();
    if(liveProcessesOnDevice > 1) {
      /* More than 1 process on device. Device Profiling for multi-process not supported yet.
       */
      std::string warnMsg = "Multiple live processes running on device. Hardware Debug and Profiling data will be unavailable for this process.";
      std::cout << warnMsg << std::endl;
//      xrt_core::message::send(xrt_core::message::severity_level::XRT_WARNING, "XRT", warnMsg) ;
      mIsDeviceProfiling = false;
      mIsDebugIPlayoutRead = true;
      return;
    }

    std::ifstream ifs(path.c_str(), std::ifstream::binary);
    if(!ifs) {
      return;
    }
    char buffer[65536];
    // debug_ip_layout max size is 65536
    ifs.read(buffer, 65536);
    debug_ip_layout *map;
    if (ifs.gcount() > 0) {
      map = (debug_ip_layout*)(buffer);
      for( unsigned int i = 0; i < map->m_count; i++ ) {
      switch(map->m_debug_ip_data[i].m_type) {
        case AXI_MM_MONITOR :        aimList.push_back(new AIM(mDevice, i, &(map->m_debug_ip_data[i])));
                                     break;
        case ACCEL_MONITOR  :        amList.push_back(new AM(mDevice, i, &(map->m_debug_ip_data[i])));
                                     break;
        case AXI_STREAM_MONITOR :    asmList.push_back(new ASM(mDevice, i, &(map->m_debug_ip_data[i])));
                                     break;
        case AXI_MONITOR_FIFO_LITE : fifoCtrl = new TraceFifoLite(mDevice, i, &(map->m_debug_ip_data[i]));
                                     break;
        case AXI_MONITOR_FIFO_FULL : fifoRead = new TraceFifoFull(mDevice, i, &(map->m_debug_ip_data[i]));
                                     break;
        case AXI_TRACE_FUNNEL :      traceFunnel = new TraceFunnel(mDevice, i, &(map->m_debug_ip_data[i]));
                                     break;
        case TRACE_S2MM :            traceDMA = new TraceS2MM(mDevice, i, &(map->m_debug_ip_data[i]));
                                     break;
        default : break;
        // case AXI_STREAM_PROTOCOL_CHECKER

      }
     }
    }
    ifs.close();

#if 0
    for(auto mon : aimList) {
        mon->showProperties();
    }

    for(auto mon : amList) {
        mon->showProperties();
    }

    for(auto mon : asmList) {
        mon->showProperties();
    }
    if(fifoCtrl) fifoCtrl->showProperties();
    if(fifoRead) fifoRead->showProperties();
    if(traceDMA) traceDMA->showProperties();
    if(traceFunnel) traceFunnel->showProperties();
#endif

    mIsDebugIPlayoutRead = true;
  }

  void DeviceIntf::configureDataflow(bool* ipConfig)
  {
    // this ipConfig only tells whether the corresponding CU has ap_control_chain :
    // could have been just a property on the monitor set at compile time (in debug_ip_layout)
    if(!ipConfig)
      return;

    uint32_t i = 0;
    for(auto mon: amList) {
        mon->configureDataflow(ipConfig[i++]);
    }
  }

  void DeviceIntf::configAmContext(const std::string& ctx_info)
  {
    if (ctx_info.empty())
      return;
    for (auto mon : amList) {
      mon->disable();
    }
  }

  void DeviceIntf::initTS2MM(uint64_t bufSz, uint64_t bufAddr)
  {
    traceDMA->init(bufSz, bufAddr);
  }
  
  uint64_t DeviceIntf::getWordCountTs2mm()
  {
    return traceDMA->getWordCount();
  }

  uint8_t DeviceIntf::getTS2MmMemIndex()
  {
    return traceDMA->getMemIndex();
  }

  void DeviceIntf::resetTS2MM()
  {
    traceDMA->reset();
  }

  void DeviceIntf::parseTraceData(void* traceData, uint64_t bytes, xclTraceResultsVector& traceVector)
  {
    traceDMA->parseTraceBuf(traceData, bytes, traceVector);
  }

} // namespace xdp
