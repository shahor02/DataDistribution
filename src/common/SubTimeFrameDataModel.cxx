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

#include "ReadoutDataModel.h"
#include "SubTimeFrameDataModel.h"

#include "DataDistLogger.h"

#include <map>
#include <iterator>
#include <algorithm>

namespace o2::DataDistribution
{

using namespace o2::header;

////////////////////////////////////////////////////////////////////////////////
/// SubTimeFrame
////////////////////////////////////////////////////////////////////////////////
SubTimeFrame::SubTimeFrame(uint64_t pStfId)
  : mHeader(pStfId)
{
  updateCreationTimeMs(); // set the current creation time
}

// Reindex split payload parts
void SubTimeFrame::updateStf() const
{
  if (mDataUpdated) {
    return;
  }

  // recalculate data size
  mDataSize = 0;

  // Update data block indexes
  for (auto &lIdentSubSpecVect : mData) {
    StfSubSpecMap &lSubSpecMap = lIdentSubSpecVect.second;

    for (auto &lSubSpecMsgVector : lSubSpecMap) {
      StfDataVector &lMsgVector = lSubSpecMsgVector.second;

      // make sure all messages are updated
      // note: each vector can contain mix of single- or split-payload messages
       for (auto &lStfMsg : lMsgVector) {

        if (!lStfMsg.mHeader) {
          EDDLOG("BUG: unexpected null header in STF size={}", lStfMsg.mDataParts.size());
          continue;
        }

        if (lStfMsg.mDataParts.empty()) {
          EDDLOG("BUG: no data in StfMessage");
          continue;
        }

        auto lDataHdr = lStfMsg.getDataHeaderMutable();
        if (!lDataHdr) {
          EDDLOG("BUG: unexpected null header in STF size={}", lStfMsg.mDataParts.size());
          continue;
        }

        // update tf meta to all data headers
        lStfMsg.setTfCounter_RunNumber(mHeader.mId, mHeader.mRunNumber);
        // update first orbit if not present in the data (old tf files)
        // update tfCounter
        if (mHeader.mFirstOrbit != std::numeric_limits<std::uint32_t>::max()) {
          lStfMsg.setFirstOrbit(mHeader.mFirstOrbit);
        }

        // sum up data size (header data)
        for (auto &lDataMsg : lStfMsg.mDataParts) {
          mDataSize += lDataMsg->GetSize();
        }

        // update the split payload counters
        const auto cNumParts = lStfMsg.mDataParts.size();
        if (cNumParts > 1) {
          lDataHdr->splitPayloadIndex = cNumParts;
          lDataHdr->splitPayloadParts = cNumParts;
        } else if (cNumParts == 1) {
          lDataHdr->splitPayloadIndex = 0;
          lDataHdr->splitPayloadParts = 1;
        } else {
          EDDLOG("BUG: SubTimeFrame::updateStf(): zero data parts");
        }
      }
    }
  }
  mDataUpdated = true;
}

std::vector<EquipmentIdentifier> SubTimeFrame::getEquipmentIdentifiers() const
{
  std::vector<EquipmentIdentifier> lKeys;

  for (const auto& lDataIdentMapIter : mData) {
    for (const auto& lSubSpecMapIter : lDataIdentMapIter.second) {
      lKeys.emplace_back(lDataIdentMapIter.first, lSubSpecMapIter.first);
    }
  }

  return lKeys;
}

void SubTimeFrame::mergeStf(std::unique_ptr<SubTimeFrame> pStf, const std::string &mStfSenderId)
{
  // incoming empty STF
  if ((pStf->header().mOrigin == Header::Origin::eNull) && (pStf->getDataSize() == 0)) {
    return; // nothing to do for an empty STF
  }

  // are we starting with an empty STF? ... use the next valied one for the header
  if (mHeader.mOrigin == Header::Origin::eNull) {
    mHeader = pStf->header();
  }

  // make sure header values match
  if (mHeader.mOrigin != pStf->header().mOrigin) {
    EDDLOG_RL(5000, "Merging STFs error: STF origins do not match origin={} new_origin={} new_stfs_id={}",
      mHeader.mOrigin,  pStf->header().mOrigin, mStfSenderId);
  }

  if (mHeader.mFirstOrbit != pStf->header().mFirstOrbit) {
    EDDLOG_RL(5000,"Merging STFs error: STF first orbits do not match firstOrbit={} new_firstOrbit={} diff={} new_stfs_id={}",
      mHeader.mFirstOrbit,  pStf->header().mFirstOrbit, (std::int64_t(pStf->header().mFirstOrbit) - std::int64_t(mHeader.mFirstOrbit)),
      mStfSenderId);
  }

  // make sure data equipment does not repeat
  std::set<EquipmentIdentifier> lUnionSet;
  for (const auto& lId : getEquipmentIdentifiers()) {
    lUnionSet.emplace(lId);
  }

  for (const auto& lId : pStf->getEquipmentIdentifiers()) {
    if (lUnionSet.emplace(lId).second == false /* not inserted */) {
      IDDLOG_RL(5000, "Merging STFs error: Equipment already present: fee={} new_stfs_id={}", lId.info(), mStfSenderId);
    }
  }

  // merge the Stfs
  for (auto& lDataIdentMapIter : pStf->mData) {
    for (auto& lSubSpecMapIter : lDataIdentMapIter.second) {
      // source
      const DataIdentifier& lDataId = lDataIdentMapIter.first;
      const DataHeader::SubSpecificationType& lSubSpec = lSubSpecMapIter.first;
      StfDataVector& lStfDataVec = lSubSpecMapIter.second;

      // destination
      StfDataVector& lDstStfDataVec = mData[lDataId][lSubSpec];

      std::move(
        lStfDataVec.begin(),
        lStfDataVec.end(),
        std::back_inserter(lDstStfDataVec));
    }
  }

  // delete pStf
  pStf.reset();
}

} /* o2::DataDistribution */
