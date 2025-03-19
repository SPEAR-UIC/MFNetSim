#!/bin/bash

# We are in the experiments folder
CUR_DIR="$PWD"
CONFDIR=${CUR_DIR}/conf
POSTFIX=$1                  # prompt for postfix for experiment outputs

BINARY=$(realpath "../install/mfnetsim/bin/client-mul-wklds")
IND_DIR=${CUR_DIR}/ind${POSTFIX}
WKLD_DIR=${CUR_DIR}/wkld${POSTFIX}

# Update paths to interconnect configurations
if [[ "$(uname)" == "Darwin" ]]; then
    # macOS-specific
    sed -i '' "s|\[path-to-experiment\]|$CUR_DIR|g" $CONFDIR/network/dally-1056-min.conf
    sed -i '' "s|\[path-to-experiment\]|$CUR_DIR|g" $CONFDIR/network/dally-1056-par.conf
else
    # Linux-specific
    sed -i "s|\[path-to-experiment\]|$CUR_DIR|g" $CONFDIR/network/dally-1056-min.conf
    sed -i "s|\[path-to-experiment\]|$CUR_DIR|g" $CONFDIR/network/dally-1056-par.conf
fi


APPS=( "milc" "nekbone" "checkpoint" "cosmo")
ALLOC=("p1" "p1" "p2" "p3")
WKLDS=("wkld2" "wkld3")
ROUTINGS=("min" "par")
PLACEMENTS=("cont" "rand_node0")

# Run Independent Experiments
# Check if the folder does not exist
if [ ! -d "$IND_DIR" ]; then
    mkdir -p "$IND_DIR"
else
    echo "Folder already exists: $IND_DIR"
fi
pushd $IND_DIR

for i in "${!APPS[@]}"; do
  for ROUTING in "${ROUTINGS[@]}"; do
    for PLACEMENT in "${PLACEMENTS[@]}"; do
        $BINARY --sync=1 --workload-conf-file=$CONFDIR/workload/${APPS[$i]}.conf --rank-alloc-file=$CONFDIR/alloc/$PLACEMENT-1d-1056-300${ALLOC[$i]}.conf --codes-config=$CONFDIR/network/dally-1056-$ROUTING.conf --lp-io-dir=$PLACEMENT-$ROUTING-${APPS[$i]} --lp-io-use-suffix=1
    done
  done
done

popd

# Run Workload Experiments
# Check if the folder does not exist
if [ ! -d "$WKLD_DIR" ]; then
    mkdir -p "$WKLD_DIR"
else
    echo "Folder already exists: $WKLD_DIR"
fi
pushd $WKLD_DIR

for WKLD in "${WKLDS[@]}"; do
  for ROUTING in "${ROUTINGS[@]}"; do
    for PLACEMENT in "${PLACEMENTS[@]}"; do
        $BINARY --sync=1 --workload-conf-file=$CONFDIR/workload/$WKLD.conf --rank-alloc-file=$CONFDIR/alloc/$PLACEMENT-1d-1056-mixwkld.conf --codes-config=$CONFDIR/network/dally-1056-$ROUTING.conf --lp-io-dir=$PLACEMENT-$ROUTING-$WKLD --lp-io-use-suffix=1
    done
  done
done

popd



