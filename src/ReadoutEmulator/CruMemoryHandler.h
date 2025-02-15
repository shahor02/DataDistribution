// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#ifndef ALICEO2_CRU_MEMORY_HANDLER_H_
#define ALICEO2_CRU_MEMORY_HANDLER_H_

#include <ConcurrentQueue.h>
#include <ReadoutDataModel.h>

#include "Headers/DataHeader.h"

#include <fairmq/FairMQUnmanagedRegion.h>

#include <stack>
#include <map>
#include <vector>
#include <queue>
#include <iterator>

#include <mutex>
#include <condition_variable>
#include <thread>

#include <functional>

#include <fairmq/FwdDecls.h>

namespace o2::DataDistribution
{

using namespace o2::header;

struct CRUSuperpage {
  char* mDataVirtualAddress;
  char* mDataBusAddress;
};

struct CruDmaPacket {
  FairMQUnmanagedRegion* mDataSHMRegion = nullptr;
  char* mDataPtr = nullptr;
  size_t mDataSize = size_t(0);
};

struct ReadoutLinkO2Data {
  ReadoutSubTimeframeHeader mLinkHeader;
  std::vector<CruDmaPacket> mLinkRawData;
};

class CruMemoryHandler
{
 public:
  CruMemoryHandler() = default;
  ~CruMemoryHandler()
  {
    teardown();
  }

  void init(FairMQUnmanagedRegion* pDataRegion, std::size_t pSuperPageSize);
  void teardown();

  std::size_t getSuperpageSize() const
  {
    return mSuperpageSize;
  }

  // get a superpage from the free list
  bool getSuperpage(CRUSuperpage& sp);

  // get number of superpages from the free list (perf)
  template <class OutputIt>
  std::size_t getSuperpages(unsigned long n, OutputIt spDst)
  {
    return mSuperpages.try_pop_n(n, spDst);
  }

  // not useful
  void put_superpage(const char* spVirtAddr);

  // address must match shm fairmq messages sent out
  void get_data_buffer(const char* dataBufferAddr, const std::size_t dataBuffSize);
  void put_data_buffer(const char* dataBufferAddr, const std::size_t dataBuffSize);
  size_t free_superpages();

  auto getDataRegion() const
  {
    return mDataRegion;
  }

  char* getDataRegionPtr() const
  {
    return reinterpret_cast<char*>(
      (reinterpret_cast<uintptr_t>(mDataRegion->GetData()) + mSuperpageSize - 1)  & ~(mSuperpageSize - 1)
    );
  }

  auto getDataRegionSize() const
  {
    return mDataRegion->GetSize();
  }

  // FIFO of filled ReadoutLinkO2Data updates to be sent to STFBuilder (thread safe)
  // linkThread<1..N> -> queue -> mCruO2InterfaceThread
  void putLinkData(ReadoutLinkO2Data&& pLinkData)
  {
    mO2LinkDataQueue.push(std::move(pLinkData));
  }
  bool getLinkData(ReadoutLinkO2Data& pLinkData)
  {
    return mO2LinkDataQueue.pop(pLinkData);
  }

 private:
  FairMQUnmanagedRegion* mDataRegion;

  std::size_t mSuperpageSize;

  /// stack of free superpages
  ConcurrentLifo<CRUSuperpage> mSuperpages;

  /// used buffers
  /// split tracking for scalability into several buckets based on sp address
  static constexpr unsigned long cBufferBucketSize = 127;

  struct BufferBucket {
    std::mutex mLock;
    std::unordered_map<const char*, CRUSuperpage> mVirtToSuperpage;
    // map<sp_address, map<buff_addr, buf_len>>
    std::unordered_map<const char*, std::unordered_map<const char*, std::size_t>> mUsedSuperPages;
  };

  BufferBucket mBufferMap[cBufferBucketSize];

  BufferBucket& getBufferBucket(const char* pAddr)
  {
    return mBufferMap[std::hash<const char*>{}(pAddr) % cBufferBucketSize];
  }

  /// output data queue
  ConcurrentFifo<ReadoutLinkO2Data> mO2LinkDataQueue;
};

} /* namespace o2::DataDistribution */

#endif /* ALICEO2_CRU_MEMORY_HANDLER_H_ */
