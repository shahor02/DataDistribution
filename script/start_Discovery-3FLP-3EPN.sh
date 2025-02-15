#!/usr/bin/env bash

set -u

SCRIPT="$(realpath $0)"
SCRIPTPATH="$(dirname $SCRIPT)"

chainConfig="${SCRIPTPATH}/discovery-flp-epn-chain.json"
readoutConfig="${SCRIPTPATH}/readout_cfg/readout_emu.cfg"

function parse_parameters() {
read -d '' PARSER <<"EOF"
import argparse

parser = argparse.ArgumentParser(description='Run FLP-EPN chain (for testing only)',
                                 formatter_class=argparse.ArgumentDefaultsHelpFormatter)
parser.add_argument('--readout', dest='readout', required=False, action='store_true', help='Use o2-readout-exe data source in emulation mode.')

parser.add_argument('--tmux', required=False, action='store_true', help='Run in tmux CC mode')
parser.add_argument('--no-gui', dest='nogui', required=False, action='store_true', help='Show GUI')

parser.add_argument('-f', '--flp', required=False, default=1, action='store', type=int, help='Number of FLP instances (1,2,3)')
parser.add_argument('-e', '--epn', required=False, default=1, action='store', type=int, help='Number of EPN instances (0,1,2,3)')
parser.add_argument('-n', '--equip', required=False, default=2, action='store', type=int, help='Number data producer equipment per FLP chain')
parser.add_argument('-r', '--rate', required=False, default=0.1, action='store', type=float, help='Data rate of each equipment instance (in Gb/s)')
parser.add_argument('-s', '--rsize', required=False, default=2, action='store', type=int, help='Size of the readout memory segment (in GiB)')
parser.add_argument('-p', '--spsize', required=False, default=2, action='store', type=int, help='Size of the readout super-pages (in MiB)')

parser.add_argument('--flp-dpl-channel', required=False, default='', action='store', help='Name of the DPL input channel on FLP for STFs.')
parser.add_argument('--epn-dpl-channel', required=False, default='', action='store', help='Name of the DPL input channel on EPN for STFs.')

parser.add_argument('--stf-builder-sink-dir', required=False, default='', action='store', help='Dir where to store STFs from SubTimeFrameBuilder')
parser.add_argument('--tf-builder-sink-dir', required=False, default='', action='store', help='Dir where to store TFs from TimeFrameBuilder')

parser.add_argument('--flp-netif', required=True, default='', action='store', help='Name of the FLP network interface to use.')
parser.add_argument('--epn-netif', required=True, default='', action='store', help='Name of the EPN network interface to use.')

parser.add_argument('--detector', required=False, default='TPC', action='store', help='Name of detector for StfBuilder (uppercase).')
parser.add_argument('--check-rdh', required=False, default='', action='store_true', help='Check all RDH headers.')

try:
  args = parser.parse_args()
except SystemExit:
  exit(1) # return error to stop the script if help is shown

print("EMU=%s" % ("readout" if args.readout else "emu"))
print("FLP_CNT=%s" % args.flp)
print("EPN_CNT=%s" % args.epn)
print("EQUIPMENT_CNT=%s" % args.equip)
print("EQUIPMENT_RATE=%s" % int(args.rate * 1e+9))
print("DATA_REGION_SIZE=%s" % int(args.rsize * (1<<30)))
print("SUPERPAGE_SIZE=%s" % int(args.spsize * (1<<20)))
print("STF_BUILDER_DPL_CHAN='%s'" % args.flp_dpl_channel)
print("TF_BUILDER_DPL_CHAN='%s'" % args.epn_dpl_channel)
print("GUI=%s" % ("" if args.nogui else "--gui"))
print("STF_BUILDER_SINK_DIR='%s'" % args.stf_builder_sink_dir)
print("TF_BUILDER_SINK_DIR='%s'" % args.tf_builder_sink_dir)

print("FLP_NETIF='%s'" % args.flp_netif)
print("EPN_NETIF='%s'" % args.epn_netif)

print("DETECTOR='%s'" % args.detector)
print("CHECKRDH=%s" % ("--rdh-sanity-check" if args.check_rdh else ""))

print("USE_TMUX=%s" % ("1" if args.tmux else ""))
EOF

python3 -c "$PARSER" "$@"
}

ARGS="$(parse_parameters "$@")"
if [ ! $? -eq 0 ]; then
  echo "$ARGS"
  exit 1
fi

echo "$ARGS"
eval "$ARGS"

# make sure consul is running
docker start consul-datadist
# delete everything
docker exec -it consul-datadist consul kv delete -recurse /epn/data-dist/request/
docker exec -it consul-datadist consul kv delete -recurse /epn/data-dist/partition/

# start processes and then add a new partition request
PARTITION="##=test_partition=##"

# new request info
FLP_ID_LIST=""
# REQ_ID=$(openssl rand -hex 4)

IO_THREADS=8

if [[ "$EMU" == "emu" ]]; then
  READOUT="ReadoutEmulator"
  READOUT+=" --transport shmem"
  READOUT+=" --shm-monitor true"
  READOUT+=" --mq-config $chainConfig"
  READOUT+=" --data-shm-region-size $DATA_REGION_SIZE"
  READOUT+=" --cru-superpage-size $SUPERPAGE_SIZE"
  READOUT+=" --cru-link-count $EQUIPMENT_CNT"
  READOUT+=" --cru-link-bits-per-s $EQUIPMENT_RATE"
  READOUT+=" $GUI"
  READOUT+=" --io-threads $IO_THREADS"

  READOUT_PARAM_0="--link-id-offset 0    --id readout-0 --session default"
  READOUT_PARAM_1="--link-id-offset 1000 --id readout-1 --session flp-s1"
  READOUT_PARAM_2="--link-id-offset 2000 --id readout-2 --session flp-s2"
else
  echo "Using o2-readout-exe in emulation mode. Configuration is read from $readoutConfig"
  echo "Make sure the Readout is installed or the Readout module is loaded."
  echo "Only 1 FLP chain can be emulated when using the readout source."
  FLP_CNT=1

  READOUT="o2-readout-exe"
  READOUT+=" file://$readoutConfig"

  READOUT_PARAM_0=""
  READOUT_PARAM_1=""
  READOUT_PARAM_2=""
fi

STF_BUILDER="StfBuilder"
STF_BUILDER+=" --transport shmem"
STF_BUILDER+=" --shm-monitor true"
STF_BUILDER+=" --mq-config $chainConfig"
STF_BUILDER+=" $CHECKRDH"
STF_BUILDER+=" --io-threads $IO_THREADS"
STF_BUILDER+=" --detector $DETECTOR"
STF_BUILDER+=" --detector-rdh 4"
STF_BUILDER+=" --monitoring-interval=1.0"
STF_BUILDER+=" --monitoring-log"
STF_BUILDER+=" --discovery-partition=$PARTITION"
# STF_BUILDER+=" --discovery-endpoint=http://localhost:8500"
STF_BUILDER+=" --discovery-endpoint=no-op://localhost:8500"
# STF_BUILDER+=" --run-type=topology"

# use DPL serialization on FLPs
STF_BUILDER+=" --dpl-channel-name=dpl-stf-channel"


if [[ ! -z $STF_BUILDER_SINK_DIR ]]; then
  if [[ ! -d $STF_BUILDER_SINK_DIR ]]; then
    echo "STF Builder file sink directory does not exist!"
    exit 1
  fi

  STF_BUILDER+=" --data-sink-enable"
  STF_BUILDER+=" --data-sink-dir $STF_BUILDER_SINK_DIR"
  STF_BUILDER+=" --data-sink-max-stfs-per-file 44"
  STF_BUILDER+=" --data-sink-sidecar"
fi

STF_SENDER="StfSender"
STF_SENDER+=" --discovery-net-if=$FLP_NETIF"
STF_SENDER+=" --discovery-partition=$PARTITION"
STF_SENDER+=" --discovery-endpoint=http://localhost:8500"
STF_SENDER+=" --mq-config $chainConfig"
STF_SENDER+=" --monitoring-interval=2.0"
STF_SENDER+=" --monitoring-log"
STF_SENDER+=" --shm-monitor=false"

TF_BUILDER="TfBuilder"
TF_BUILDER+=" --discovery-net-if=$EPN_NETIF"
TF_BUILDER+=" --discovery-partition=$PARTITION"
TF_BUILDER+=" --discovery-endpoint=http://localhost:8500"
TF_BUILDER+=" --mq-config $chainConfig"
TF_BUILDER+=" --monitoring-interval=5.0"
TF_BUILDER+=" --monitoring-log"
TF_BUILDER+=" --stand-alone"

TF_BUILDER+=" --shm-monitor true"
TF_BUILDER+=" --tf-data-region-size $(( 32 << 10 ))"
# TF_BUILDER+=" --tf-data-region-id 2"


if [[ ! -z $TF_BUILDER_DPL_CHAN ]]; then
  TF_BUILDER+=" --dpl-channel-name=$TF_BUILDER_DPL_CHAN"
fi

if [[ ! -z $TF_BUILDER_SINK_DIR ]]; then
  if [[ ! -d $TF_BUILDER_SINK_DIR ]]; then
    echo "TF Builder file sink directory does not exist!"
    exit 1
  fi

  TF_BUILDER+=" --data-sink-enable"
  TF_BUILDER+=" --data-sink-dir $TF_BUILDER_SINK_DIR"
  # TF_BUILDER+=" --data-sink-max-stfs-per-file 44"
  TF_BUILDER+=" --data-sink-max-stfs-per-file=500" # 500 files -> there are small RawTFs (500B)
  TF_BUILDER+=" --data-sink-max-file-size=500"     # 500 MiB file
  TF_BUILDER+=" --data-sink-sidecar"
fi

TF_SCHEDULER="TfScheduler"
TF_SCHEDULER+=" --discovery-net-if=$EPN_NETIF"
TF_SCHEDULER+=" --discovery-endpoint=http://localhost:8500"
TF_SCHEDULER+=" --monitoring-interval=2.0"
TF_SCHEDULER+=" --monitoring-log"

echo "$STF_BUILDER"
echo "$STF_SENDER"
echo "$TF_SCHEDULER"
echo "$TF_BUILDER"

# export DATADIST_DEBUG_DPL_CHAN=1
export DATADIST_NEW_DPL_CHAN=1
# export UCX_LOG_LEVEL=req
export UCX_TLS=all

if [[ -z $USE_TMUX ]]; then

  # start TF scheduler
  xterm -geometry 120x60+2240+0 -hold -e "$TF_SCHEDULER --discovery-id=sched0 --id tf_scheduler --session sched-session" &

  # (EPN) Start TimeFrameBuilders
  if [[ $EPN_CNT -gt 0 ]]; then
    xterm -geometry 90x20+1680+0 -hold -e "$TF_BUILDER --discovery-id=epn0 --id tf_builder-0 --session epn-s0" &
  fi
  if [[ $EPN_CNT -gt 1 ]]; then
    xterm -geometry 90x20+1680+300 -hold -e "$TF_BUILDER --discovery-id=epn1 --id tf_builder-1 --session epn-s1" &
  fi
  if [[ $EPN_CNT -gt 2 ]]; then
    xterm -geometry 90x20+1680+600 -hold -e0 "$TF_BUILDER --discovery-id=epn2 --id tf_builder-2 --session epn-s2" &
  fi

  # (FLP) Start FLP processes
  if [[ $FLP_CNT -gt 0 ]]; then
    xterm -geometry 90x57+1120+0 -hold -e "$STF_SENDER --discovery-id=flp0 --id stf_sender-0  --session default" &
    FLP_ID_LIST+="flp0"
    xterm -geometry 90x57+560+0 -hold -e "$STF_BUILDER --id stf_builder-0 --session default" &
    xterm -geometry 90x57+0+0 -hold -e "$READOUT $READOUT_PARAM_0" &
  fi
  if [[ $FLP_CNT -gt 1 ]]; then
    xterm -geometry 90x57+1120+0 -hold -e "$STF_SENDER --discovery-id=flp1 --id stf_sender-1  --session flp-s1" &
    FLP_ID_LIST+=";flp1"
    xterm -geometry 90x57+560+0 -hold -e "$STF_BUILDER --id stf_builder-1 --session flp-s1" &
    xterm -geometry 90x57+0+0 -hold -e "$READOUT $READOUT_PARAM_1" &
  fi
  if [[ $FLP_CNT -gt 2 ]]; then
    xterm -geometry 90x57+1120+0 -hold -e "$STF_SENDER --discovery-id=flp2 --id stf_sender-2  --session flp-s2" &
    FLP_ID_LIST+=";flp2"
    xterm -geometry 90x57+560+0 -hold -e "$STF_BUILDER --id stf_builder-2 --session flp-s2" &
    xterm -geometry 90x57+0+0 -hold -e "$READOUT $READOUT_PARAM_2" &
  fi


else
  # poor man's tmux environment cloning; make sure you're running tmux in control center (-CC) mode
  ENV_VAR_FILE=$(mktemp)
  typeset -gx > $ENV_VAR_FILE

  # NOTE: does not work with the updated configuration
  # cat $ENV_VAR_FILE
  if [[ $EPN_CNT -gt 0 ]]; then
    FLP_ID_LIST+="flp0"
    tmux -CC \
      new-window \
      "source $ENV_VAR_FILE; $TF_SCHEDULER --discovery-id=sched0 --id tf_scheduler --session sched-session; read" \; \
      split-window \
      "source $ENV_VAR_FILE; $READOUT $READOUT_PARAM_0; read" \; \
      split-window \
      "source $ENV_VAR_FILE; $STF_BUILDER --id stf_builder-0 --session default; read" \; \
      split-window  \
      "source $ENV_VAR_FILE; $STF_SENDER --id stf_sender-0 --discovery-id=flp0 --session default; read" \; \
      split-window  \
      "source $ENV_VAR_FILE; numactl --interleave=all $TF_BUILDER --id tf_builder-0 --discovery-id=epn0 --session epn-s0; read" \; \
      select-layout even-horizontal
  fi

  if [[ $FLP_CNT -gt 1 ]]; then
    FLP_ID_LIST+=";flp1"
    tmux -CC \
      new-window \
      "source $ENV_VAR_FILE; $READOUT $READOUT_PARAM_1; read" \; \
      split-window \
      "source $ENV_VAR_FILE; $STF_BUILDER --id stf_builder-1 --session flp-s1; read" \; \
      split-window  \
      "source $ENV_VAR_FILE; $STF_SENDER --id stf_sender-1 --discovery-id=flp1 --session  flp-s1; read" \; \
      select-layout even-horizontal
  fi

  # NOTE: does not work with the updated configuration
  # cat $ENV_VAR_FILE
  if [[ $EPN_CNT -gt 1 ]]; then
    tmux -CC \
      split-window \
      "source $ENV_VAR_FILE; DATADIST_SHM_ZERO_CHECK=1 numactl --interleave=all $TF_BUILDER --id tf_builder-1 --discovery-id=epn1 --session epn-s1; read" \; \
      select-layout even-horizontal

      # "source $ENV_VAR_FILE; numactl --interleave=all gdb --args $TF_BUILDER --id tf_builder-1 --discovery-id=epn1 --session epn-s1; read" \; \
  fi

  sleep 2
  rm "$ENV_VAR_FILE"
fi


# send request to scheduler to create a new partition
docker exec -it consul-datadist consul kv put epn/data-dist/request/partition-id "$PARTITION"
docker exec -it consul-datadist consul kv put epn/data-dist/request/stf-sender-id-list "$FLP_ID_LIST"
