#!/usr/bin/python
from operator import truediv
import numpy as np
import string
import matplotlib.pyplot as plt
import subprocess
import os
import sys
import shlex

scale=1000*1000    #byte to MB or ns to millisec

COLOR = [['dimgrey', 'darkgray'],['darkred', 'lightsalmon'],
         ['darkgreen', 'palegreen'],['darkblue', 'lightsteelblue'],
         ['darkgoldenrod', 'khaki']]
LINE_WIDTH = 1.5
FONT_SIZE = 20
BOX_SPACING=2

legend1 = ['baseline', 'workload1', 'workload2']
legend2 = ['baseline', 'workload1']
legend3 = ['baseline', 'workload2']

ALLOC = ['cont', 'rand']
ROUT = ['par', 'min']

WORKLOAD=[['Baseline', 'Workload1', 'Workload2'], 
          ['Baseline', 'Workload1', 'Workload2'], 
          ['Baseline', 'Workload1'], 
          ['Baseline', 'Workload2']]

APPS=['Checkpoint', 'Cosmoflow', 'MILC', 'Nekbone' ]

XLABLE = ['cont-par', 'rand-par', 'cont-min', 'rand-min']


def get_color(wkld_names, copies):
    indexes = []
    # print wkld_names
    for i in range(copies):
        for name in wkld_names:
            index = -1
            for ch in name:
                if ch in string.digits:
                    index=int(ch)
            if name=='Baseline':
                index = 0
            if name=='MixedWkld':
                index = 4
            if index==-1:
                index = 0
            indexes.append(index)
    return indexes

def getdata(datafile):

    # header = open(wkld_msg_stats_file, 'r').readline()
    rawdata = np.genfromtxt(datafile, delimiter=",", skip_header=2)

    data = rawdata.view(np.float64).reshape(len(rawdata), -1) 
    data = data/scale  
    # print data[:, 0]
    return data


def drawboxplot(data, appname, ylabel, figfile):

    num_wlkd = len(WORKLOAD[APPS.index(appname)])

    xticks = [((num_wlkd+1)/2.0)+i*(num_wlkd+BOX_SPACING) for i in range(0, len(XLABLE)) ]
    # print xticks

    positions=[]
    start_pos=1
    for i in range(len(ROUT)*len(ALLOC)):
        for j in range(num_wlkd):
            positions.append(start_pos+j)
        start_pos += num_wlkd + BOX_SPACING

    # print positions
    subplot = 100
    fig = plt.figure(subplot)
    fig.set_size_inches(8, 4)
    ax1 = fig.add_subplot(111)

    colors_indexes = get_color(WORKLOAD[APPS.index(appname)], len(ALLOC)*len(ROUT))
    # print colors_indexes

    # boxplot
    bp = ax1.boxplot(data,  positions = positions, 
            showmeans=True, meanprops={"markerfacecolor":"red", "markeredgecolor":"red"},
            patch_artist=True, widths=0.8)

    idx=0
    for box in bp['boxes']:
        box.set( color=COLOR[colors_indexes[idx]][0], linewidth=LINE_WIDTH)
        box.set( facecolor = COLOR[colors_indexes[idx]][1])
        idx += 1
    idx=0
    for whisker in bp['whiskers']:
        whisker.set(color=COLOR[colors_indexes[idx/2]][0], linewidth=LINE_WIDTH)
        idx += 1
    idx=0
    for cap in bp['caps']:
        cap.set(color=COLOR[colors_indexes[idx/2]][0], linewidth=LINE_WIDTH)
        idx += 1
    idx=0            
    for median in bp['medians']:
        median.set(color=COLOR[colors_indexes[idx]][0], linewidth=LINE_WIDTH+1)       
        idx += 1
    idx=0                        
    for flier in bp['fliers']:
        flier.set(marker='o', markersize=2, color=COLOR[colors_indexes[idx/2]][0], alpha=0.8)
        idx += 1

    # barplot
    # bar_width = 1
    # for i in range(len(positions)):
    #     plt.bar(positions[i], np.max(data[:,i]), bar_width, alpha=0.8, color=COLOR[colors_indexes[i]][1])

    # print average
    idx = 0
    for wkld in WORKLOAD[APPS.index(appname)]:
        for xlb in XLABLE:
            data[idx]
            if len(data[idx]) !=0:
                avg = sum(data[idx])/float(len(data[idx]))
            else:
                avg = sum(data[idx])
            # print wkld+' '+xlb+' '+' Avg: '+str(avg)
            idx += 1


    for i in range(len(xticks)):
        xticks[i] += 0.5 

    ax1.set_xticks(xticks)
    ax1.set_xticklabels(XLABLE, fontsize=FONT_SIZE-3)
    ax1.set_ylabel(ylabel, fontsize=FONT_SIZE-3)
    ax1.get_xaxis().tick_bottom()
    ax1.get_yaxis().tick_left()

    if appname=='checkpoint-latency.csv':
        ax1.set_ylim((-0.5, 3))  # for checkpoint

    # ax1.set_ylim((-0, 1900))

    plt.yticks(fontsize=FONT_SIZE-3)

    # boxplot: create legend
    num_colors = len(colors_indexes)/(len(ALLOC)*len(ROUT))
    patches = []
    for i in range(num_colors):
        patches.append(bp["boxes"][i])
    # ax1.legend(patches, WORKLOAD[APPS.index(appname)], loc='best', ncol=num_colors,fontsize=9,  labelspacing=0.5)
    ax1.legend(patches, WORKLOAD[APPS.index(appname)], bbox_to_anchor=(0, 1.02, 1, 0.2), loc="lower left",
                mode="expand", borderaxespad=0, ncol=num_colors, fontsize=FONT_SIZE-3,  labelspacing=0.5)

    # barplot
    # ax1.legend(WORKLOAD[APPS.index(appname)],loc='best', ncol=num_wlkd, fontsize=9,  labelspacing=0.5)

    plt.tight_layout()
    plt.savefig(figfile+'.eps', bbox_inches='tight', format='eps', dpi=1000)
    # plt.show()


def drawbarplot(data, appname, ylabel, figfile):

    num_wlkd = len(WORKLOAD[APPS.index(appname)])

    xticks = [((num_wlkd+1)/2.0)+i*(num_wlkd+BOX_SPACING) for i in range(0, len(XLABLE)) ]
    # print xticks

    positions=[]
    start_pos=1
    for i in range(len(ROUT)*len(ALLOC)):
        for j in range(num_wlkd):
            positions.append(start_pos+j)
        start_pos += num_wlkd + BOX_SPACING

    # print positions
    subplot = 100
    fig = plt.figure(subplot)
    fig.set_size_inches(8, 4)
    ax1 = fig.add_subplot(111)

    colors_indexes = get_color(WORKLOAD[APPS.index(appname)], len(ALLOC)*len(ROUT))
    # print colors_indexes

    # barplot
    bar_width = 1
    for i in range(len(positions)):
        plt.bar(positions[i], np.max(data[:,i]), bar_width, alpha=0.8, color=COLOR[colors_indexes[i]][1], edgecolor = "black", linewidth=LINE_WIDTH)

    for i in range(len(xticks)):
        xticks[i] += 0.5 

    ax1.set_xticks(xticks)
    ax1.set_xticklabels(XLABLE, fontsize=FONT_SIZE-3)
    ax1.set_ylabel(ylabel, fontsize=FONT_SIZE-3)
    ax1.get_xaxis().tick_bottom()
    ax1.get_yaxis().tick_left()

    ax1.set_ylim((-0.5, ax1.get_ylim()[1]))

    plt.yticks(fontsize=FONT_SIZE-3)

    # barplot
    # ax1.legend(WORKLOAD[APPS.index(appname)],loc='best', ncol=num_wlkd, fontsize=FONT_SIZE-3,  labelspacing=0.5)
    ax1.legend(WORKLOAD[APPS.index(appname)], bbox_to_anchor=(0, 1.02, 1, 0.2), loc="lower left",
                mode="expand", borderaxespad=0, ncol=num_wlkd, fontsize=FONT_SIZE-3,  labelspacing=0.5)

    plt.tight_layout()
    plt.savefig(figfile+'.eps', bbox_inches='tight', format='eps', dpi=1000)
    # plt.show()    

if __name__ == "__main__":
    datafile = sys.argv[1]
    appname = sys.argv[2]
    ylabel = sys.argv[3]
    figfile = sys.argv[4]
    figtype = sys.argv[5]

    # subprocess.call(shlex.split("./getappfromwkld.sh "+app+" "+hasSyn))
    data = getdata(datafile)
    if figtype=='box':
        drawboxplot(data, appname, ylabel, figfile)
    elif figtype=='bar':
        drawbarplot(data, appname, ylabel, figfile)
    else:
        print("Syntax error. Choose bar or box")

