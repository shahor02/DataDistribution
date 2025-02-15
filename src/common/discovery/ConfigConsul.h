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

#ifndef ALICEO2_DATADIST_CONFIGCONSUL_H_
#define ALICEO2_DATADIST_CONFIGCONSUL_H_

#include "Config.h"
#include "ConfigParameters.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "discovery.pb.h"
#pragma GCC diagnostic pop

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <ppconsul/kv.h>
#include <string>
#include <variant>
#include <mutex>

namespace o2::DataDistribution
{

namespace ConsulImpl {

std::string to_string(const ProcessTypePB pType);
std::tuple<std::string, std::uint64_t> getCurrentTimeString();

class BasicInfo;
class PartitionInfo;

using ppconsul::kv::Kv;
using ppconsul::kv::KeyValue;

template<typename T>
class ConsulConfig : public Config {

public:
  ConsulConfig() = delete;

  ConsulConfig(const ProcessType pProcessType, const std::string &pEndpoint, const bool pRequired = true)
  : Config(pProcessType), mEndpoint(pEndpoint)
  {
    const bool lEmpty = pEndpoint.empty();
    const bool lNoOp = boost::starts_with(pEndpoint, "no-op");

    // if not configured and not needed
    if (!pRequired && (lEmpty || lNoOp)) {
      IDDLOG("Not connecting to a consul instance.");
      mConsul = nullptr;
      return;
    }

    // error if not provided and required
    if (pRequired && lEmpty) {
      if (pProcessType == ProcessType::StfBuilder) {
        EDDLOG("Consul endpoint is required for StfSender production use. Use 'discovery-endpoint=no-op://' for testing.");
      } else {
        EDDLOG("Consul endpoint is mandatory for {}.", pProcessType.to_string());
      }
      throw std::invalid_argument("discovery-endpoint parameter is not provided");
    }

    if (lNoOp) {  // Support for CI setups without consul
      WDDLOG("Consul endpoint is configured as 'no-op'. Use only for testing!");

      if (pRequired &&
        ( pProcessType == ProcessType::StfSender ||
          pProcessType == ProcessType::TfBuilder ||
          pProcessType == ProcessType::TfScheduler) ) {
        throw std::invalid_argument("Error: A valid discovery-endpoint (consul) parameter must be provided.");
      }

      IDDLOG("Not connecting to a consul instance.");
      mConsul = nullptr;
      return;
    }

    try {
      mConsul = std::make_unique<ppconsul::Consul>(mEndpoint);
      IDDLOG("Connecting to Consul. endpoint={}", mEndpoint);

      // thread for fetching the tunables
      mPollThread = create_thread_member("consul_params", &ConsulConfig::ConsulPollingThread, this);
      // wait for tunables to be available
      using namespace std::chrono_literals;
      while (!mTunablesRead.load()) { std::this_thread::sleep_for(10ms); }
    } catch (std::exception &err) {
      mConsul = nullptr;
      EDDLOG("Error while connecting to Consul. endpoint={} what={}", mEndpoint, err.what());
    }
  }

  bool enabled() const { return (mConsul != nullptr); }

  ConsulConfig(ConsulConfig &&) = default;

  virtual ~ConsulConfig() { cleanup(); }

  bool write(bool pInitial = false)
  {
    if (!mConsul || !createKeyPrefix()) {
      return false;
    }

    auto [lTimeStr, lTimet] = getCurrentTimeString();
    mStatus.mutable_info()->set_last_update(lTimeStr);
    mStatus.mutable_info()->set_last_update_t(lTimet);

    std::string lData;
    mStatus.SerializeToString(&lData);

    // persist global partition info
    if constexpr (std::is_same_v<T, TfSchedulerConfigStatus>) {
      using namespace std::string_literals;
      static const std::string sPartStateKey = getInfoPrefix(mStatus.partition().partition_id());
      try {
        Kv kv(*mConsul);
        kv.set(sPartStateKey + "/partition-state"s, std::to_string(mStatus.partition_state()) );
        kv.set(sPartStateKey + "/partition-state-str"s, PartitionState_Name(mStatus.partition_state()));
        kv.set(sPartStateKey + "/last-update"s, lTimeStr);
      } catch (std::exception &e) {
        EDDLOG("Consul kv::set error. what={}", e.what());
      }
    }

    return write_string(lData, pInitial);
  }

  void cleanup()
  {
    mRunning = false;
    mTunableCV.notify_all();

    if (mPollThread.joinable()) {
      mPollThread.join();
    }

    if (!mConsul || mConsulKey.empty()) {
      return; // nothing was written
    }

    {
      std::scoped_lock lLock(mConsulLock);
      try {
        Kv kv(*mConsul);
        DDDLOG("Erasing DataDistribution discovery key: {}", mConsulKey);
        kv.eraseAll(mConsulKey);
      } catch (std::exception &e) {
        EDDLOG("Consul kv erase error. what={}", e.what());
        EDDLOG("Unable to cleanup the DataDistribution discovery configuration.");
      }
    }
  }

  T& status() { return mStatus; }

private:

  bool createKeyPrefix();

  bool write_string(const std::string &lData, const bool pInitial = false)
  {
    if (!mConsul) {
      return false;
    }

    std::unique_ptr<ppconsul::kv::Kv> kv;

    std::scoped_lock lLock(mConsulLock);

    try {
      kv = std::make_unique<ppconsul::kv::Kv>(*mConsul);
    } catch (std::exception &e) {
      EDDLOG("Consul kv init error. what={}", e.what());
      return false;
    }

    try{
      if (pInitial) {
        // make sure the key does not exist before
        if (kv->count(mConsulKey) > 0) {
          EDDLOG("Consul kv error, the key is already present: {}", mConsulKey);
          return false;
        }
      }

      kv->set(mConsulKey, lData);
    } catch (std::exception &e) {
      EDDLOG("Consul kv set error. what={}", e.what());
      return false;
    }

    return true;
  }


  std::string mEndpoint;

  std::mutex mConsulLock;
  std::unique_ptr<ppconsul::Consul> mConsul;
  std::string mConsulKey;

  // tunable polling thread
  std::atomic_bool mRunning = true;
  std::atomic_bool mTunablesRead = false;
  std::thread mPollThread;
  mutable std::mutex mTunablesLock;
    std::condition_variable mTunableCV;
    std::map<std::string, std::string> mTunables;
    std::map<std::string, std::string> mTunablesToAdd; // Add missing default values


  T mStatus;
private:

  static
  const std::string getInfoPrefix(const std::string pPartId)
  {
    using namespace std::string_literals;
    return "epn/data-dist/partition/"s + pPartId + "/info"s;
  }
  static
  constexpr std::string getGlobalTunablePrefix() {
    using namespace std::string_literals;
    return "epn/data-dist/parameters/Global/"s;
  }

  static
  constexpr std::string getTunablePrefix() {
    using namespace std::string_literals;

    if constexpr (std::is_same_v<T, TfSchedulerConfigStatus>) {
      return "epn/data-dist/parameters/TfScheduler/"s;
    }

    if constexpr (std::is_same_v<T, StfBuilderConfigStatus>) {
      return "epn/data-dist/parameters/StfBuilder/"s;
    }

    if constexpr (std::is_same_v<T, StfSenderConfigStatus>) {
      return "epn/data-dist/parameters/StfSender/"s;
    }

    if constexpr (std::is_same_v<T, TfBuilderConfigStatus>) {
      return "epn/data-dist/parameters/TfBuilder/"s;
    }

    static_assert (
      !std::is_same_v<T, TfSchedulerConfigStatus> ||
      !std::is_same_v<T, StfBuilderConfigStatus>  ||
      !std::is_same_v<T, StfSenderConfigStatus>   ||
      !std::is_same_v<T, TfBuilderConfigStatus> , "wrong Config Type");
    return ""s;
  }

  void ConsulPollingThread()
  {
    using namespace std::chrono_literals;
    using namespace std::string_literals;

    const std::vector<std::string> keyVec = { getGlobalTunablePrefix(), getTunablePrefix() };

    while (mRunning) {
      std::unique_lock lLock(mTunablesLock);
      const auto lPrevValues = std::move(mTunables);
      mTunables.clear();

      try {
        std::scoped_lock lKVLock(mConsulLock);

        for (const auto &lKey : keyVec) {
          Kv kv(*mConsul);
          // Get per Proces keys
          std::vector<KeyValue> lReqItems = kv.items(lKey);

          if (lReqItems.empty()) {
            kv.set(lKey, ""s);
          } else {

            for (const auto &lKeyVal : lReqItems) {
              if (lKeyVal.valid() && !lKeyVal.value.empty()) {
                DDDLOG("Parameters: prefix={} key={} val={}", lKey, lKeyVal.key, lKeyVal.value);

                // remove full key prefix
                const auto lParamName = lKeyVal.key.substr(lKey.length());

                if ((lPrevValues.count(lParamName)) > 0 && (lPrevValues.at(lParamName) != lKeyVal.value)) {
                  IDDLOG("Consul: Updating parameter {}. old_value={} new_value={}"s,
                    lParamName, lPrevValues.at(lParamName), lKeyVal.value);
                } else if (lPrevValues.count(lParamName) == 0) {
                  IDDLOG("Consul: Reading parameter {}={}", lParamName, lKeyVal.value);
                }

                mTunables[lParamName] = lKeyVal.value;
              }
            }
          }
        }

        {
          Kv kv(*mConsul);
          // write back missing parameters
          for (const auto &lKeyVal : mTunablesToAdd) {
            const auto &lKey = lKeyVal.first;
            const auto &lVal = lKeyVal.second;
            const auto lFullKey = (boost::starts_with(lKey, "DataDist") ? getGlobalTunablePrefix() : getTunablePrefix()) + lKey;
            kv.set(lFullKey, lVal);

            DDDLOG("Missing options written to Consul. key={} val={}", lFullKey, lVal);
          }
          mTunablesToAdd.clear();
        }

      } catch (std::exception &e) {
        WDDLOG_ONCE("Consul kv param retrieve error. what={}", e.what());
      }

      mTunablesRead = true;
      mTunableCV.wait_for(lLock, 30s);
    }

    DDDLOG("Exiting params ConsulPollingThread.");
  }

public:
  inline bool getBoolParam(const std::string_view &pKeySv, const bool pDefault)
  {
    const std::string lKey(pKeySv);

    std::unique_lock lLock(mTunablesLock);
    if (mTunables.count(lKey) == 0) {
      mTunablesToAdd.insert_or_assign(lKey, (pDefault ? "true" : "false"));
      return pDefault;
    } else {
      const auto &lVal = mTunables.at(lKey);
      return (lVal == "true" || lVal == "TRUE" || lVal == "True" ||lVal == "1") ? true : false;
    }
  }

  inline std::string getStringParam(const std::string_view &pKeySv, const std::string_view &pDefault)
  {
    const std::string lKey(pKeySv);

    std::unique_lock lLock(mTunablesLock);
    if (mTunables.count(lKey) == 0) {
      mTunablesToAdd[lKey] = pDefault;
      return std::string(pDefault);
    } else {
      return mTunables.at(lKey);
    }
  }

  inline std::int64_t getInt64Param(const std::string_view &pKeySv, const std::int64_t pDefault)
  {
    const std::string lKey(pKeySv);

    std::unique_lock lLock(mTunablesLock);
    if (mTunables.count(lKey) == 0) {
      mTunablesToAdd[lKey] = std::to_string(pDefault);
      return pDefault;
    } else {

      try {
        return boost::lexical_cast<std::int64_t>(mTunables.at(lKey));
      } catch( boost::bad_lexical_cast const &e) {
        mTunablesToAdd[lKey] = std::to_string(pDefault);
        EDDLOG("Error parsing consul parameter (int64) {}. str_value={} what={}", pKeySv, mTunables.at(lKey), e.what());
      }
    }
    return pDefault;
  }

  inline std::uint64_t getUInt64Param(const std::string_view &pKeySv, const std::uint64_t pDefault)
  {
    const std::string lKey(pKeySv);

    std::unique_lock lLock(mTunablesLock);
    if (mTunables.count(lKey) == 0) {
      mTunablesToAdd[lKey] = std::to_string(pDefault);
      return pDefault;
    } else {

      try {
        const auto &lVal = mTunables.at(lKey);
        if (!lVal.empty() && lVal.at(0) == '-') {
          throw boost::bad_lexical_cast();
        }
        return boost::lexical_cast<std::uint64_t>(mTunables.at(lKey));
      } catch( boost::bad_lexical_cast const &e) {
        mTunablesToAdd[lKey] = std::to_string(pDefault);
        EDDLOG("Error parsing consul parameter (uint64) {}. str_value={} what={}", pKeySv, mTunables.at(lKey), e.what());
      }
    }
    return pDefault;
  }

  bool getNewPartitionRequest(PartitionRequest &pNewPartitionRequest)
  {
    using namespace std::string_literals;

    static const std::string sReqKeyPrefix = "epn/data-dist/request"s;
    static const std::string sInvalidKeyPrefix = "epn/data-dist/invalid_requests/"s; // + "time_t"

    static const std::string sTimeSubKey = "/request-time"s;

    static const std::string sPartitionIdSubKey = "/partition-id"s;
    static const std::string sStfSenderListSubKey = "/stf-sender-id-list"s;


    static const std::string sReqPartitionIdKey   = sReqKeyPrefix + sPartitionIdSubKey;
    static const std::string sReqStfSenderListKey = sReqKeyPrefix + sStfSenderListSubKey;

    static_assert(std::is_same_v<T, TfSchedulerConfigStatus>, "Only TfScheduler can call this method.");

    bool lReqValid = false;

    // check for 'epn/data-dist/request/partition-name' key
    try {
      std::scoped_lock lLock(mConsulLock);

      Kv kv(*mConsul);

      std::vector<KeyValue> lReqItems;

      do {
        lReqItems = kv.items(sReqKeyPrefix);

        if (lReqItems.size() == 0) {
          return false;
        }

        if (lReqItems.size() < 2) {
          DDDLOG("Incomplete partition request, retrying...");
          return false;
        }

        if (lReqItems.size() == 2) {
          // get the request fields
          auto lPartitionIdIt = std::find_if(std::begin(lReqItems), std::end(lReqItems),
            [&] (KeyValue const& p) { return p.key == sReqPartitionIdKey; });
          if (lPartitionIdIt == std::end(lReqItems)) {
            EDDLOG("Invalid new partition request. Missing key: {}", sReqPartitionIdKey);
            break;
          }

          auto lFlpIdList = std::find_if(std::begin(lReqItems), std::end(lReqItems),
            [&] (KeyValue const& p) { return p.key == sReqStfSenderListKey; });
          if (lFlpIdList == std::end(lReqItems)) {
            EDDLOG("Invalid new partition request. Missing key: {}", sReqStfSenderListKey);
            break;
          }

          // validate the request fields
          // partition name, check if already exist
          const std::string lPartitionId = boost::trim_copy(lPartitionIdIt->value);
          if (lPartitionId.empty()) {
            EDDLOG("Invalid new partition request. Partition (ID) cannot be empty.");
            break;
          }

          // validate the flp list
          std::vector<std::string> lStfSenderIds;
          const std::string lStfSenderIdsReq = lFlpIdList->value;

          // split, trim, remove empty
          boost::split(lStfSenderIds, lStfSenderIdsReq, boost::is_any_of(";,\n\t\r "), boost::token_compress_on);
          const auto lNumStfSendersReq = lStfSenderIds.size();

          // sort and unique
          std::sort(std::begin(lStfSenderIds), std::end(lStfSenderIds));
          lStfSenderIds.erase( std::unique(std::begin(lStfSenderIds), std::end(lStfSenderIds)), std::end(lStfSenderIds));

          if (lStfSenderIds.empty()) {
            EDDLOG("Invalid new partition request. List of StfSender IDs is empty.");
            break;
          }

          if (lNumStfSendersReq != lStfSenderIds.size()) {
            EDDLOG("Invalid new partition request. Requested FLP IDs are not unique. "
              "provided_num={} unique_num={}", lNumStfSendersReq, lStfSenderIds.size());
            break;
          }

          pNewPartitionRequest.mPartitionId = lPartitionId;
          pNewPartitionRequest.mStfSenderIdList = std::move(lStfSenderIds);

          lReqValid = true;
        }
      } while(false);

      // move the partition request if exists
      if (!lReqItems.empty()) {
        std::string lInfoPrefix;
        auto [lTimeStr, lTimet] = getCurrentTimeString();
        // build info key for valid or invalid partition request
        if (lReqValid) {
          lInfoPrefix = getInfoPrefix(pNewPartitionRequest.mPartitionId);
        } else {
          lInfoPrefix = sInvalidKeyPrefix + std::to_string(lTimet);
        }

        kv.set(lInfoPrefix + sTimeSubKey, lTimeStr);

        // move values and erase original request
        for (const auto &lKeyVal : lReqItems) {
          auto lNewKey = lInfoPrefix + lKeyVal.key.substr(sReqKeyPrefix.length());
          kv.set(lNewKey, lKeyVal.value);
          kv.erase(lKeyVal.key);
        }
      }
    } catch (std::exception &e) {
      EDDLOG("Consul kv partition retrieve error. what={}", e.what());
      EDDLOG("Unable to check for new partition requests.");
    }

    return lReqValid;
  }

  bool getStfSenderConfig(const std::string &pPartId, const std::string &pStfSenderId, StfSenderConfigStatus &pStfSenderStat /*out*/)
  {
    static const std::string sKeyPrefix = "epn/data-dist/partition/";

    const std::string lConsulKey = sKeyPrefix + pPartId + "/StfSender/" + pStfSenderId;

    try {
      std::scoped_lock lLock(mConsulLock);

      Kv kv(*mConsul);

      const auto lReqItem = kv.item(lConsulKey);

      if (!lReqItem.valid()) {
        // does not exist!
        return false;
      }

      if (lReqItem.value.empty()) {
        EDDLOG("Consul: no data returned for key: {}", lConsulKey);
        return false;
      }

      if (!pStfSenderStat.ParseFromString(lReqItem.value)) {
        EDDLOG("Cannot parse protobuf message from consul! (type StfSenderConfigStatus)");
      }

      return true;

    } catch (std::exception &e) {
      EDDLOG("Consul kv StfSender retrieve error. what={}", e.what());
    }
    return false;
  }

  bool getTfBuilderConfig(const std::string &pPartId, const std::string &pTfBuilderId, TfBuilderConfigStatus &pTfBuilderStat /*out*/)
  {
    static const std::string sKeyPrefix = "epn/data-dist/partition/";

    const std::string lConsulKey = sKeyPrefix + pPartId + "/TfBuilder/" + pTfBuilderId;

    try {
      std::scoped_lock lLock(mConsulLock);

      Kv kv(*mConsul);

      const auto lReqItem = kv.item(lConsulKey);

      if (!lReqItem.valid()) {
        EDDLOG("Consul: key does not exist: {}", lConsulKey);
        return false;
      }

      if (lReqItem.value.empty()) {
        EDDLOG("Consul: no data returned for key: {}", lConsulKey);
        return false;
      }

      if (!pTfBuilderStat.ParseFromString(lReqItem.value)) {
        EDDLOG("Cannot parse protobuf message from consul! (type StfSenderConfigStatus)");
      }

      return true;

    } catch (std::exception &e) {
      EDDLOG("Consul kv StfSender retrieve error. what={}", e.what());
    }

    return false;
  }

  bool getTfSchedulerConfig(const std::string &pPartId, TfSchedulerConfigStatus &pTfSchedulerStat /*out*/)
  {
    static constexpr const char* sKeyPrefix = "epn/data-dist/partition/";

    const std::string lConsulKey = sKeyPrefix + pPartId + "/TfScheduler";

    // get the scheduler instance with the "smallest" ID
    try {
      std::scoped_lock lLock(mConsulLock);

      Kv kv(*mConsul);

      // get partition state
      using namespace std::string_literals;
      static const std::string sPartStateKey = getInfoPrefix(mStatus.partition().partition_id());
      auto lPartState = kv.item(sPartStateKey + "/partition-state"s);
      if (lPartState.valid() && !lPartState.value.empty()) {
        pTfSchedulerStat.set_partition_state(o2::DataDistribution::PartitionState(stoul(lPartState.value)));
      }

      // get all schedulers in the partition
      auto lReqItems = kv.items(lConsulKey);
      if (lReqItems.empty()) {
        return false;
      }

      // sort lReqItems by SchedulerId
      std::sort(lReqItems.begin(), lReqItems.end(),  [](auto const& a, auto const& b){
        return a.key < b.key;
      });

      // get the scheduler with lowest ID
      if (!pTfSchedulerStat.ParseFromString(lReqItems.begin()->value)) {
        EDDLOG("Cannot parse protobuf message from consul! (type StfSenderConfigStatus)");
      }

      return true;

    } catch (std::exception &e) {
      EDDLOG("Consul kv StfSender retrieve error. what={}", e.what());
    }
    return false;
  }


};


template <class T>
bool ConsulConfig<T>::createKeyPrefix() {
  // make sure all fields are available
  if (mStatus.partition().partition_id().empty()) {
    throw std::runtime_error("createKeyPrefix: partition id must be set");
  }

  const auto &lBasic = mStatus.info();

  mConsulKey =  "epn/data-dist/partition/" +
                mStatus.partition().partition_id() + "/" +
                to_string(lBasic.type()) + "/" +
                lBasic.process_id();

  return true;
}


} /* namespace ConsulImpl */

///
///  ConsulConfig specializations for o2::DataDistribution
///

// TfScheduler
using ConsulTfScheduler = ConsulImpl::ConsulConfig<TfSchedulerConfigStatus>;

// StfBuilder
using ConsulStfBuilder = ConsulImpl::ConsulConfig<StfBuilderConfigStatus>;

// StfSender
using ConsulStfSender = ConsulImpl::ConsulConfig<StfSenderConfigStatus>;

// TfBuilder
using ConsulTfBuilder = ConsulImpl::ConsulConfig<TfBuilderConfigStatus>;


} /* namespace o2::DataDistribution */

#endif /* ALICEO2_DATADIST_CONFIGCONSUL_H_ */
