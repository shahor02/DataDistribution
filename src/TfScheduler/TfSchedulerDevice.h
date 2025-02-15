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

#ifndef ALICEO2_TF_SCHEDULER_DEVICE_H_
#define ALICEO2_TF_SCHEDULER_DEVICE_H_

#include "TfSchedulerInstance.h"

#include <ConfigConsul.h>

#include <Utilities.h>
#include <FmqUtilities.h>

#include <thread>


namespace o2
{
namespace DataDistribution
{

class ConsulConfig;

class TfSchedulerDevice : public DataDistDevice
{
 public:
  /// Default constructor
  TfSchedulerDevice();

  /// Default destructor
  ~TfSchedulerDevice() override;

  void InitTask() final;
  void ResetTask() final;

 protected:
  void PreRun() final;
  void PostRun() final;
  bool ConditionalRun() final;

  /// Scheduler Instances
  std::string mPartitionId;
  std::unique_ptr<TfSchedulerInstanceHandler> mSchedInstance;

  std::shared_ptr<ConsulTfScheduler> mDiscoveryConfig;
};
}
} /* namespace o2::DataDistribution */

#endif /* ALICEO2_TF_SCHEDULER_DEVICE_H_ */
