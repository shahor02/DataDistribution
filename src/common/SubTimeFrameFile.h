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

#ifndef ALICEO2_SUBTIMEFRAME_FILE_H_
#define ALICEO2_SUBTIMEFRAME_FILE_H_

#include <chrono>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <vector>

#include <Headers/DataHeader.h>
#include <SubTimeFrameDataModel.h>

namespace o2
{
namespace DataDistribution
{

////////////////////////////////////////////////////////////////////////////////
/// SubTimeFrameFileMeta
////////////////////////////////////////////////////////////////////////////////

struct SubTimeFrameFileMeta {
  static const o2::header::DataDescription sDataDescFileSubTimeFrame;

  static const o2::header::DataHeader getDataHeader()
  {
    auto lHdr = o2::header::DataHeader(
      SubTimeFrameFileMeta::sDataDescFileSubTimeFrame,
      o2::header::gDataOriginFLP,
      0, // subspecification: not used
      sizeof(SubTimeFrameFileMeta));

    lHdr.payloadSerializationMethod = o2::header::gSerializationMethodNone;

    return lHdr;
  }

  static constexpr std::uint64_t getSizeInFile()
  {
    return sizeof(o2::header::DataHeader) + sizeof(SubTimeFrameFileMeta);
  }

  ///
  /// Version of STF file format
  ///
  const std::uint64_t mStfFileVersion = 1;

  ///
  /// Size of the Stf in file, including this header.
  ///
  std::uint64_t mStfSizeInFile;

  ///
  /// Time when Stf was written (in ms)
  ///
  std::uint64_t mWriteTimeMs;

  auto getTimePoint()
  {
    using namespace std::chrono;
    return time_point<system_clock, milliseconds>{ milliseconds{ mWriteTimeMs } };
  }

  std::string getTimeString()
  {
    using namespace std::chrono;
    std::time_t lTime = system_clock::to_time_t(getTimePoint());

    std::stringstream lTimeStream;
    lTimeStream << std::put_time(std::localtime(&lTime), "%F %T");
    return lTimeStream.str();
  }

  SubTimeFrameFileMeta(const std::uint64_t pStfSize)
    : SubTimeFrameFileMeta()
  {
    mStfSizeInFile = pStfSize;
  }

  SubTimeFrameFileMeta()
    : mStfSizeInFile{ 0 }
  {
    using namespace std::chrono;
    mWriteTimeMs = time_point_cast<milliseconds>(system_clock::now()).time_since_epoch().count();
  }

  friend std::ostream& operator<<(std::ostream& pStream, const SubTimeFrameFileMeta& pMeta);
};

std::ostream& operator<<(std::ostream& pStream, const SubTimeFrameFileMeta& pMeta);

////////////////////////////////////////////////////////////////////////////////
/// SubTimeFrameFileDataIndex
////////////////////////////////////////////////////////////////////////////////

struct SubTimeFrameFileDataIndex {
  static const o2::header::DataDescription sDataDescFileStfDataIndex;

  struct DataIndexElem {
    /// Equipment Identifier: unrolled to pack better
    o2::header::DataDescription mDataDescription;
    o2::header::DataOrigin mDataOrigin;
    /// Number of data blocks <data_header, data>
    std::uint32_t mDataBlockCnt = 0;
    /// subspecification (u64)
    o2::header::DataHeader::SubSpecificationType mSubSpecification = 0;
    /// Offset of data block (corresponding data header) relative to
    std::uint64_t mOffset = 0;
    /// Total size of data blocks including headers
    std::uint64_t mSize = 0;

    DataIndexElem() = delete;
    DataIndexElem(const EquipmentIdentifier& pId,
                  const std::uint32_t pCnt,
                  const std::uint64_t pOff,
                  const std::uint64_t pSize)
      : mDataDescription(pId.mDataDescription),
        mDataOrigin(pId.mDataOrigin),
        mDataBlockCnt(pCnt),
        mSubSpecification(pId.mSubSpecification),
        mOffset(pOff),
        mSize(pSize)
    {
      static_assert(sizeof(DataIndexElem) == 48,
                    "DataIndexElem changed -> Binary compatibility is lost!");
    }
  };

  SubTimeFrameFileDataIndex() = default;

  void clear() noexcept { mDataIndex.clear(); }
  bool empty() const noexcept { return mDataIndex.empty(); }

  void AddStfElement(const EquipmentIdentifier& pEqDataId,
                     const std::uint32_t pCnt,
                     const std::uint64_t pOffset,
                     const std::uint64_t pSize)
  {
    mDataIndex.emplace_back(pEqDataId, pCnt, pOffset, pSize);
  }

  std::uint64_t getSizeInFile() const
  {
    return sizeof(o2::header::DataHeader) + (sizeof(DataIndexElem) * mDataIndex.size());
  }

  friend std::ostream& operator<<(std::ostream& pStream, const SubTimeFrameFileDataIndex& pIndex);

 private:
  const o2::header::DataHeader getDataHeader() const
  {
    auto lHdr = o2::header::DataHeader(
      sDataDescFileStfDataIndex,
      o2::header::gDataOriginAny,
      0, // subspecification: not used
      mDataIndex.size() * sizeof(DataIndexElem));

    lHdr.payloadSerializationMethod = o2::header::gSerializationMethodNone;

    return lHdr;
  }

  std::vector<DataIndexElem> mDataIndex;
};

std::ostream& operator<<(std::ostream& pStream, const SubTimeFrameFileDataIndex& pIndex);
}
} /* o2::DataDistribution */

#endif /* ALICEO2_SUBTIMEFRAME_FILE_H_ */
