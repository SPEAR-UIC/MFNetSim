#!/bin/bash

PYTHON_BIN="python2"

# We are in the experiments folder
CUR_DIR="$PWD"
POSTFIX=$1    # prompt for postfix for experiment outputs
IND_DIR=${CUR_DIR}/ind${POSTFIX}
WKLD_DIR=${CUR_DIR}/wkld${POSTFIX}

# prepare link stats, output link statistic files in ./linkstats/ folder
STATFOLDER="linkstats"

# Check if the folder does not exist
if [ ! -d "$STATFOLDER" ]; then
    mkdir -p "$STATFOLDER"
else
    echo "Folder already exists: $STATFOLDER"
fi

$PYTHON_BIN sep_by_appid.py $WKLD_DIR
$PYTHON_BIN sep_by_appid.py $IND_DIR

#Draw link statistics figures, as shown in Figure 11 of the paper. Figures will be placed in the linkfig folder.
LINKFIGFOLDER="linkfig"

# Check if the folder does not exist
if [ ! -d "$LINKFIGFOLDER" ]; then
    mkdir -p "$LINKFIGFOLDER"
else
    echo "Folder already exists: $LINKFIGFOLDER"
fi

$PYTHON_BIN drawlinkstat.py

# In appstats folder, [appname]-[statstype].csv contains statistics for each rank in both baseline and mixed workload cases.
# The output application figures will be placed in the appfig folder.

APPFIGFOLDER="appfig"

# Check if the folder does not exist
if [ ! -d "$APPFIGFOLDER" ]; then
    mkdir -p "$APPFIGFOLDER"
else
    echo "Folder already exists: $APPFIGFOLDER"
fi

#Draw Figure 10
$PYTHON_BIN draw.py appstats/checkpoint-latency.csv Checkpoint "Msg Latency(ms)" appfig/checkpoint-latency box
$PYTHON_BIN draw.py appstats/checkpoint-iotime.csv Checkpoint "IO_Time(ms)" appfig/checkpoint-iotime box

#Draw Figure 12
$PYTHON_BIN draw.py appstats/cosmoflow-latency.csv Cosmoflow "Msg Latency(ms)" appfig/cosmoflow-latency box
$PYTHON_BIN draw.py appstats/cosmoflow-iotime.csv Cosmoflow "IO_Time(ms)" appfig/cosmoflow-iotime box
$PYTHON_BIN draw.py appstats/cosmoflow-commtime.csv Cosmoflow "Comm_Time(ms)" appfig/cosmoflow-commtime bar

# Draw Figure 13
$PYTHON_BIN draw.py appstats/nekbone-latency.csv Nekbone "Msg Latency(ms)" appfig/nekbone-latency box
$PYTHON_BIN draw.py appstats/nekbone-commtime.csv Nekbone "Comm_Time(ms)" appfig/nekbone-commtime bar
$PYTHON_BIN draw.py appstats/milc-latency.csv MILC "Msg Latency(ms)" appfig/milc-latency box
$PYTHON_BIN draw.py appstats/milc-commtime.csv MILC "Comm_Time(ms)" appfig/milc-commtime bar


