// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#ifndef ALICEO2_READOUT_EMULATOR_DEVICE_H_
#define ALICEO2_READOUT_EMULATOR_DEVICE_H_

#include "CruEmulator.h"

#include <ReadoutDataModel.h>
#include <Utilities.h>
#include <FmqUtilities.h>

#include <memory>
#include <deque>
#include <condition_variable>

namespace o2::DataDistribution
{

class ReadoutDevice : public DataDistDevice
{
 public:
  static constexpr const char* OptionKeyOutputChannelName = "output-channel-name";

  static constexpr const char* OptionKeyReadoutDataRegionSize = "data-shm-region-size";

  static constexpr const char* OptionKeyLinkIdOffset = "link-id-offset";

  static constexpr const char* OptionKeyCruSuperpageSize = "cru-superpage-size";

  static constexpr const char* OptionKeyCruLinkCount = "cru-link-count";
  static constexpr const char* OptionKeyCruLinkBitsPerS = "cru-link-bits-per-s";

  /// Default constructor
  ReadoutDevice();

  /// Default destructor
  ~ReadoutDevice() override;

  void InitTask() final;
  void ResetTask() final;

 protected:
  bool ConditionalRun() final;
  void PreRun() final;
  void PostRun() final { };


  void InfoThread();
  void SendingThread();

  // data and Descriptor regions
  // must be here because NewUnmanagedRegionFor() is a method of FairMQDevice...
  FairMQUnmanagedRegionPtr mDataRegion;

  std::string mOutChannelName;
  std::size_t mDataRegionSize;

  std::size_t mLinkIdOffset;

  std::size_t mSuperpageSize;
  std::size_t mDmaChunkSize;
  unsigned mCruLinkCount;
  std::uint64_t mCruLinkBitsPerS;

  std::shared_ptr<CruMemoryHandler> mCruMemoryHandler;

  std::vector<std::unique_ptr<CruLinkEmulator>> mCruLinks;

  // messages to send
  std::vector<FairMQMessagePtr> mDataBlockMsgs;
  std::thread mSendingThread;

  /// Observables
  std::thread mInfoThread;
};

} /* namespace o2::DataDistribution */

#endif /* ALICEO2_READOUT_EMULATOR_DEVICE_H_ */
