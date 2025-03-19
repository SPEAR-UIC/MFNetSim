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


LINE_WIDTH = 3
FONT_SIZE = 20
BOX_SPACING=2
COLOR = {'Baseline': 'dimgray', 'Workload1':'coral', 'Workload2':'lawngreen'}
line_style = {'cont-par': 'solid', 'rand-par': 'dashed', 'cont-min':'dashdot', 'rand-min':'dotted' }


ALLOC = ['cont', 'rand']
ROUT = ['par', 'min']

ALLOC_ROUT = ['cont-par', 'rand-par', 'cont-min', 'rand-min']
WORKLOAD=[['Baseline', 'Workload1', 'Workload2'], 
          ['Baseline', 'Workload1', 'Workload2'], 
          ['Baseline', 'Workload1'], 
          ['Baseline', 'Workload2']]

APPS=['checkpoint', 'cosmo', 'milc', 'nekbone' ]
WKLDS = {'baseline': 'Baseline', 'wkld2': 'Workload1', 'wkld3': 'Workload2' }
STATSTYPEA = ['link', 'Llink', 'Glink']
STATSTYPEB = ['traffic', 'saturation']

ORDER = {'cont-par':0, 'rand-par':1, 'cont-min':2, 'rand-min':3}


DATA={}
LEGENDS = {}

def removezeros(data):
    return [i for i in data if i != 0.0 ]


def getdata(filepath='./linkstats/'):

    #construct data and legends
    for a in APPS:
        DATA[a] = {}
        LEGENDS[a] = []
        for i in ALLOC_ROUT:
            for j in WORKLOAD[APPS.index(a)]:
                thislegend = i+"-"+j
                LEGENDS[a].append(thislegend)
                DATA[a][thislegend] = {}
                for l in STATSTYPEA:
                    DATA[a][thislegend][l]={}
                    for n in STATSTYPEB:
                        DATA[a][thislegend][l][n]=[]

    # read data
    for subdir, dirs, files in os.walk(filepath):
        for file in files:
            if 'csv' not in file:
                continue
            # print file
            filename = file
            parser = filename.split("-")
            thisappname = parser[0]  
            thiswkld = parser[1]
            thisalloc = parser[2]
            thisrotr = parser[3]
            thisstats = parser[4]
            thislegend = thisalloc+"-"+thisrotr+"-"+WKLDS[thiswkld]

            # read file data
            linkstats_file = os.path.join(filepath, filename)
            rawdata = np.genfromtxt(linkstats_file, delimiter=",", dtype=None, names=['srcid', 'dstid', 'linktraffic', 'linksat'])
            data1 = rawdata['linktraffic'].view(np.float64)
            data2 = rawdata['linksat'].view(np.float64)
            data1 = data1/scale 
            data2 = data2/scale 
            DATA[thisappname][thislegend][thisstats]['traffic'] = data1
            DATA[thisappname][thislegend][thisstats]['saturation'] = data2


    # print DATA['checkpoint']['rand-par-Workload1']['link']['traffic']

def drawlinkstat(data, legends, idx, appname='checkpoint', outputpath='./linkfig/'):
    # link traffic
    for i in STATSTYPEA:
        subplot = 100+idx
        idx += 1
        fig = plt.figure(subplot)
        fig.set_size_inches(8, 4)
        ax1 = fig.add_subplot(111)

        maxlen = 0
        for elem in LEGENDS[appname]:
            thislist = data[appname][elem][i]['traffic']
            removezeros(thislist)
            if maxlen < len(thislist):
                maxlen = len(thislist)

        for item in LEGENDS[appname]:
            wkldtype = item.split("-")[-1]
            thisstyle = item.replace('-'+wkldtype, '')
            link_stats = removezeros(data[appname][item][i]['traffic'])

            router_load = [None]*(maxlen-len(link_stats)) + sorted(link_stats)
            plt.plot(router_load, label=item,
                    linewidth=LINE_WIDTH, color=COLOR[wkldtype],
                    linestyle=line_style[thisstyle]
                )
            if len(link_stats) !=0:
                avg = sum(link_stats)/float(len(link_stats))
            else:
                avg = sum(link_stats)
            # print appname+' '+item+' '+i+' Avg: '+str(avg)

        plt.xticks(fontsize=FONT_SIZE)
        plt.yticks(fontsize=FONT_SIZE)
        # plt.yscale("log", basey=2)
        #plt.ticklabel_format(style='sci', axis='y', scilimits=(0,0))#scientific notation
        axes = plt.gca()
        # axes.set_xlim(left=0)
        # Hide the right and top spines
        axes.spines['right'].set_visible(False)
        axes.spines['top'].set_visible(False)
        # Only show ticks on the left and bottom spines
        axes.yaxis.set_ticks_position('left')
        axes.xaxis.set_ticks_position('bottom')
        plt.grid(color='whitesmoke')

        # axes.set_xlim((0, axes.get_xlim()[1]))

        plt.xlabel('Router ports sorted by traffic', fontsize=FONT_SIZE)
        plt.ylabel(i.title()+' Traffic (MB)', fontsize=FONT_SIZE)

        title = appname.title()+'-'+i.title()+'-Traffic'
        # plt.title(title , fontsize=FONT_SIZE)
        # plt.legend(loc = 'best', fontsize=FONT_SIZE)
        plt.tight_layout()
        # print 'save figure: '+str(subplot)+' '+title
        plt.savefig(outputpath+title+'.eps', format='eps', dpi=1000)
        # plt.show()
        plt.cla()
        plt.close(fig)

    # link saturation
    for ii in STATSTYPEA:
        subplot = 100+idx
        idx += 1
        fig = plt.figure(subplot)
        fig.set_size_inches(8, 4)
        # fig.set_size_inches(12, 4) # for legend
        ax1 = fig.add_subplot(111)

        for item in LEGENDS[appname]:
            wkldtype = item.split("-")[-1]
            thisstyle = item.replace('-'+wkldtype, '')
            link_stats = data[appname][item][ii]['saturation']
            link_stats.sort()


            yvals = np.arange(len(link_stats))/float(len(link_stats))
            plt.plot(link_stats, yvals*100, label=item,
                    linewidth=LINE_WIDTH, color=COLOR[wkldtype],
                    linestyle=line_style[thisstyle]
                    )

            # sumsat = sum(link_stats)
            # print appname+' '+item+' '+i+' Big 5: '
            # print link_stats[-5:-1]
        
        plt.xticks(fontsize=FONT_SIZE)
        plt.yticks(fontsize=FONT_SIZE)
        #plt.ticklabel_format(style='sci', axis='y', scilimits=(0,0))#scientific notation
        axes = plt.gca()

        # print axes.get_xlim()
        ax1.set_xlim(axes.get_xlim())

        # axes.set_xlim(left=0)
        # plt.ylim(90, 100)
        # Hide the right and top spines
        axes.spines['right'].set_visible(False)
        axes.spines['top'].set_visible(False)
        # Only show ticks on the left and bottom spines
        axes.yaxis.set_ticks_position('left')
        axes.xaxis.set_ticks_position('bottom')
        plt.grid(color='whitesmoke')

        plt.xlabel(ii.title()+" Saturation Time (ms)", fontsize=FONT_SIZE)
        plt.ylabel('Percentage of ports', fontsize=FONT_SIZE)

        title = appname.title()+'-'+ii.title()+'-Saturation'
        # plt.title(title,  fontsize=FONT_SIZE)

        # legend
        # pos = axes.get_position()
        # fig.subplots_adjust(right=0.6)
        # axes.legend(loc='center right', bbox_to_anchor=(1.6, 0.5), fontsize=FONT_SIZE-6)

        plt.tight_layout()
        # print 'save figure: '+str(subplot)+' '+title
        plt.savefig(outputpath+title+'.eps', format='eps', dpi=1000)

        # plt.show()
        plt.cla()
        plt.close(fig)

    return idx

if __name__ == "__main__":

    getdata()
    idx = 0
    if len(sys.argv) > 1:
        appname = sys.argv[1]
        idx = drawlinkstat(DATA, LEGENDS, idx, appname)
    else:
        for appname in APPS:
            idx = drawlinkstat(DATA, LEGENDS, idx, appname)

