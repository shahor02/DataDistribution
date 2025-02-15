 // This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "TfBuilderInput.h"
#include "TfBuilderRpc.h"
#include "TfBuilderDevice.h"

#include <TfSchedulerRpcClient.h>

#include <SubTimeFrameDataModel.h>
#include <SubTimeFrameVisitors.h>

#include <DataDistributionOptions.h>
#include <DataDistMonitoring.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>

namespace o2::DataDistribution
{

using namespace std::chrono_literals;

TfBuilderInput::TfBuilderInput(TfBuilderDevice& pStfBuilderDev, std::shared_ptr<ConsulTfBuilder> pConfig, std::shared_ptr<TfBuilderRpcImpl> pRpc, unsigned pOutStage)
    : mDevice(pStfBuilderDev),
      mConfig(pConfig),
      mRpc(pRpc),
      mOutStage(pOutStage)
  {
    // Select which backend is used
    mStfRequestQueue = std::make_shared<ConcurrentQueue<std::string>>();
    mReceivedDataQueue = std::make_shared<ConcurrentQueue<ReceivedStfMeta>>();

    auto lTransportOpt = mConfig->getStringParam(DataDistNetworkTransportKey, DataDistNetworkTransportDefault);
    if (lTransportOpt == "fmq" || lTransportOpt == "FMQ" || lTransportOpt == "fairmq" || lTransportOpt == "FAIRMQ") {
      mInputFairMQ = std::make_unique<TfBuilderInputFairMQ>(pRpc, pStfBuilderDev.TfBuilderI(), *mStfRequestQueue, *mReceivedDataQueue);
    } else {
      mInputUCX = std::make_unique<TfBuilderInputUCX>(mConfig, pRpc, pStfBuilderDev.TfBuilderI(), *mStfRequestQueue, *mReceivedDataQueue);
    }
  }

bool TfBuilderInput::start()
{
  // make max number of listening channels for the partition
  mDevice.GetConfig()->SetProperty<int>("io-threads", (int) std::min(std::thread::hardware_concurrency(), 32u));
  auto lTransportFactory = FairMQTransportFactory::CreateTransportFactory("zeromq", "", mDevice.GetConfig());

  // start the input stage
  if (mInputFairMQ) {
    mInputFairMQ->start(mConfig, lTransportFactory);
  }
  if (mInputUCX) {
    mInputUCX->start();
  }

  // Start all the threads
  mState = RUNNING;

  // Start the pacer thread
  mReceivedDataQueue->start();
  mStfPacingThread = create_thread_member("tfb_pace", &TfBuilderInput::StfPacingThread, this);

  // Start the deserialize thread
  {
    std::unique_lock<std::mutex> lQueueLock(mStfMergerQueueLock);
    mStfMergeMap.clear();

    mStfDeserThread = create_thread_member("tfb_deser", &TfBuilderInput::StfDeserializingThread, this);
  }

  // Start the merger
  mStfsForMerging.start();
  mStfMergerThread = create_thread_member("tfb_merge", &TfBuilderInput::StfMergerThread, this);

  // finally start accepting TimeFrames
  mRpc->startAcceptingTfs();

  return true;
}

void TfBuilderInput::stop()
{
  // first stop accepting TimeFrames
  mRpc->stopAcceptingTfs();
  mState = TERMINATED;

  // stop FaiMQ input
  if (mInputFairMQ) {
    mInputFairMQ->stop(mConfig);
  }

  // stop UCX input
  if (mInputUCX) {
    mInputUCX->stop();
  }

  //Wait for pacer thread
  mReceivedDataQueue->stop();
  if (mStfPacingThread.joinable()) {
    mStfPacingThread.join();
  }

  // Make sure the deserializer stopped
  {
    DDDLOG("TfBuilderInput::stop: Stopping the STF merger thread.");
    {
      std::unique_lock<std::mutex> lQueueLock(mStfMergerQueueLock);
      mStfMergeMap.clear();
      IDDLOG("TfBuilderInput::stop: Merger queue emptied.");
      triggerStfMerger();
    }

    if (mStfDeserThread.joinable()) {
      mStfDeserThread.join();
    }
  }

  //Wait for merger thread
  mStfsForMerging.stop();
  if (mStfMergerThread.joinable()) {
    mStfMergerThread.join();
  }

  DDDLOG("TfBuilderInput::stop: Merger thread stopped.");
  DDDLOG("TfBuilderInput: Teardown complete.");
}

/// TF building pacing thread: rename topo TF ids, and queue physics TFs for merging
void TfBuilderInput::StfPacingThread()
{
  // Deserialization object (stf ID)
  IovDeserializer lStfReceiver(mDevice.TfBuilderI());

  while (mState == RUNNING) {

    auto lData = mReceivedDataQueue->pop();
    if (lData == std::nullopt) {
      continue;
    }
    auto &lStfInfo = lData.value();

    std::uint64_t lTfId = lStfInfo.mStfId;

    if (lStfInfo.mType == ReceivedStfMeta::MetaType::ADD) {
      // only record the intent to build a TF
      std::unique_lock<std::mutex> lQueueLock(mStfMergerQueueLock);
      assert (mStfMergeMap.count(lTfId) == 0);
      assert (mStfMergeMap.empty() || (mStfMergeMap.rbegin()->first < lTfId));

      mStfMergeMap[lTfId].reserve(mNumStfSenders);
      continue;
    } else if (lStfInfo.mType == ReceivedStfMeta::MetaType::DELETE) {
      // remove tf merge intent if no StfSenders were contacted
      std::unique_lock<std::mutex> lQueueLock(mStfMergerQueueLock);
      assert (mStfMergeMap.count(lTfId) == 1);
      assert (mStfMergeMap[lTfId].empty());

      mStfMergeMap.erase(lTfId);
      continue;
    }

    assert (lStfInfo.mType == ReceivedStfMeta::MetaType::INFO);
    assert (lStfInfo.mRecvStfdata);

    // Rename STF id if this is a Topological TF
    if (lStfInfo.mStfOrigin == SubTimeFrame::Header::Origin::eReadoutTopology) {
      // deserialize here to be able to rename the stf
      lStfInfo.mStf = std::move(lStfReceiver.deserialize(*lStfInfo.mRecvStfHeaderMeta.get(), *lStfInfo.mRecvStfdata));
      lStfInfo.mRecvStfdata = nullptr;

      const std::uint64_t lNewTfId = mRpc->getIdForTopoTf(lStfInfo.mStfSenderId, lStfInfo.mStfId);

      DDDLOG_RL(5000, "Deserialized STF. stf_id={} new_id={}", lStfInfo.mStf->id(), lNewTfId);
      lStfInfo.mStf->updateId(lNewTfId);
      lTfId = lNewTfId;
      lStfInfo.mStfId = lNewTfId;
    }


    // TfScheduler should manage memory of the region and not overcommit the TfBuilders
    { // Push the STF into the merger queue
      std::unique_lock<std::mutex> lQueueLock(mStfMergerQueueLock);

      if (lTfId > mMaxMergedTfId) {
        mStfMergeMap[lTfId].push_back(std::move(lStfInfo));
      } else {
        EDDLOG_RL(1000, "StfPacingThread: Received STF ID is smaller than the last built STF. stfs_id={} stf_id={} last_stf_id={}",
          lStfInfo.mStfSenderId, lTfId, mMaxMergedTfId);

        // reordered or duplicated STF ? Cleanup the merge map.
        mStfMergeMap.erase(lTfId);
      }
    }
    // wake up the merging thread
    triggerStfMerger();
  }

  DDDLOG("Exiting StfPacingThread.");
}


void TfBuilderInput::deserialize_headers(std::vector<ReceivedStfMeta> &pStfs)
{
  // Deserialization object
  IovDeserializer lStfReceiver(mDevice.TfBuilderI());

  for (auto &lStfInfo : pStfs) {
    if (lStfInfo.mStf) {
      continue; // already deserialized
    }

    // deserialize the data
    lStfInfo.mStf = std::move(lStfReceiver.deserialize(*lStfInfo.mRecvStfHeaderMeta.get(), *lStfInfo.mRecvStfdata));
    lStfInfo.mRecvStfdata = nullptr;
  }
}

/// FMQ->STF thread
/// This thread can block waiting on free O2 Header memory
void TfBuilderInput::StfDeserializingThread()
{
  // Deserialization object
  IovDeserializer lStfReceiver(mDevice.TfBuilderI());

  while (mState == RUNNING) {

    std::unique_lock<std::mutex> lQueueLock(mStfMergerQueueLock);
    mStfMergerCondition.wait_for(lQueueLock, 10ms, [this]{ return mStfMergerRun.load(); });
    mStfMergerRun = false;

    if (mStfMergeMap.empty()) {
      continue; // re-evaluate conditions
    }

    //  Try to complete TF with the smallest ID
    auto lMinTfInfo = mStfMergeMap.begin();
    const auto lStfId = lMinTfInfo->first;
    auto &lStfVector = lMinTfInfo->second;

    // deserialize headers of any new STFs
    deserialize_headers(lStfVector);

    // Check the number of expected STFs
    const auto lNumStfsOpt = mRpc->getNumberOfStfs(lStfId);
    if (lNumStfsOpt == std::nullopt) {
      // STF request thread has not finished requesting all STFs yet
      triggerStfMerger();
      continue;
    }

    // Check if the TF is completed
    if ((lStfVector.size() == lNumStfsOpt)) {
      mStfsForMerging.push(std::move(lStfVector));

      mStfMergeMap.erase(lStfId);
      mStfMergeCountMap.erase(lStfId);
      mRpc->setNumberOfStfs(lStfId, std::nullopt);
    }

    // TODO: add timeout for non-completed TFs?
  }

  IDDLOG("Exiting stf deserializer thread.");
}

/// STF->TF Merger thread
void TfBuilderInput::StfMergerThread()
{
  using namespace std::chrono_literals;
  using hres_clock = std::chrono::high_resolution_clock;

  auto lRateStartTime = hres_clock::now();

  std::uint64_t lNumBuiltTfs = 0;

  while (mState == RUNNING) {

    auto lStfVectorOpt = mStfsForMerging.pop();
    if (lStfVectorOpt == std::nullopt) {
      continue;
    }
    auto &lStfVector = lStfVectorOpt.value();

    // merge the current TF!
    const std::chrono::duration<double, std::milli> lBuildDurationMs =
      lStfVector.rbegin()->mTimeReceived - lStfVector.begin()->mTimeReceived;

    // start from the first element (using it as the seed for the TF)
    std::unique_ptr<SubTimeFrame> lTf = std::move(lStfVector.begin()->mStf);
    if (!lTf) {
      EDDLOG("StfMergerThread: First Stf is null. (not deserialized?)");
      continue;
    }

    // Add the rest of STFs
    for (auto lStfIter = std::next(lStfVector.begin()); lStfIter != lStfVector.end(); ++lStfIter) {
      lTf->mergeStf(std::move(lStfIter->mStf), lStfIter->mStfSenderId);
    }
    lNumBuiltTfs++;

    const auto lTfId = lTf->id();

    { // Push the STF into the merger queue
      std::unique_lock<std::mutex> lQueueLock(mStfMergerQueueLock);
      mMaxMergedTfId = std::max(mMaxMergedTfId, lTfId);
    }

    // account the size of received TF
    mRpc->recordTfBuilt(*lTf);

    DDDLOG_RL(1000, "Building of TF completed. tf_id={:d} duration_ms={} total_tf={:d}",
      lTfId, lBuildDurationMs.count(), lNumBuiltTfs);

    {
      const auto lNow = hres_clock::now();
      const auto lTfDur = std::chrono::duration<double>(lNow - lRateStartTime);
      lRateStartTime = lNow;
      const auto lRate = (1.0 / lTfDur.count());

      DDMON("tfbuilder", "tf_input.size", lTf->getDataSize());
      DDMON("tfbuilder", "tf_input.rate", lRate);
      DDMON("tfbuilder", "data_input.rate", (lRate * lTf->getDataSize()));

      DDMON("tfbuilder", "merge.receive_span_ms", lBuildDurationMs.count());
    }

    // Queue out the TF for consumption
    mDevice.queue(mOutStage, std::move(lTf));
  }

  IDDLOG("Exiting STF merger thread.");
}

} /* namespace o2::DataDistribution */
