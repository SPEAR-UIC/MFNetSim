#!/usr/bin/python
from operator import truediv
import numpy as np
import matplotlib.pyplot as plt
import subprocess
import os
import sys
import shlex

APPS=['Checkpoint', 'Cosmoflow', 'MILC', 'Nekbone' ]

ALLOC = ['cont', 'rand']
ROUT = ['par', 'min']

WKLD = {'wkld2':['milc', 'checkpoint', 'cosmo'],
        'wkld3':['nekbone', 'checkpoint', 'cosmo']}

apporder = {'milc':0, 'nekbone':0, 'checkpoint':1, 'cosmo':2}

def sep_app_msg_stats(num_apps, path):

    for subdir in next(os.walk(path))[1]:
        lp_output_folder = os.path.join(path, subdir)

        wkld_msg_stats_file = os.path.join(lp_output_folder, 'dragonfly-cn-stats')
        app_mpi_replay_stats_file = os.path.join(lp_output_folder, 'mpi-replay-stats')
        # app_io_stats_file = os.path.join(lp_output_folder, 'checkpoint-client-stats')

        #<LP id> <Workload type> <client id> <Bytes written> <Bytes read> <Synthetic data Received> <Time to write bytes > <Total elapsed time>

        # header = open(wkld_msg_stats_file, 'r').readline()
        wkld_msg_stats = np.genfromtxt(wkld_msg_stats_file, delimiter=None, skip_header=0, names=['lpid', 'tid', 'datasend', 'datarecv', 'avglatency', 'maxlatency', 'minlatency', 'packets', 'avghop','busytime'])

        #the APP.csv file is got by "grep "APP 0/1/2" mpi-replay-stats > APP.csv "
        app_mpi_replay_stats = np.genfromtxt(app_mpi_replay_stats_file, delimiter=None, names=['lpid', 'tid', 'jobid', 'rankid', 'totalsends', 'totalrecvs', 'bytessend', 'bytesrecvd','sendtime', 'commtime', 'comptime','synavgmsg','synmaxmsg'])

        # app_io_stats = np.genfromtxt(app_io_stats_file, delimiter=None, names=['lpid', 'appname', 'tid', 'writebytes','readbytes','syndata','writetime','elapsedtime'])

        # app_io_stats = np.genfromtxt(app_io_stats_file, delimiter=None, names=['lpid', 'appname', 'tid', 'writebytes','syndata','writetime','elapsedtime'])

        #'tid' in wkld_msg_stats is correspondint to 'tid' in app_mpi_replay_stats
        #transfrom ndarray to 2D array
        wkld = wkld_msg_stats.view(np.float64).reshape(len(wkld_msg_stats), -1)
        app  = app_mpi_replay_stats.view(np.float64).reshape(len(app_mpi_replay_stats), -1)
        # io = app_io_stats.view(np.float64).reshape(len(app_io_stats), -1)

        # for appid in range(num_apps):
        #     perapp = app[np.in1d(app[:,2], appid)]
        #     app_nwid = perapp[:, 1]#get the 'tid' of each app
        #     #according to nwid in application,  get corresponding rows of each app from workload data
        #     app_msg_stats = wkld[np.in1d(wkld[:,1], app_nwid)]
        #     #  print np.array_equal(app_msg_stats[:, 1],app_nwid)
        #     app_msg_stats_file = os.path.join(lp_output_folder, "app"+str(appid)+"-msg-stats.csv")
        #     with open(app_msg_stats_file, 'w') as outputfile:
        #         # outputfile.write(header)
        #         np.savetxt(outputfile, app_msg_stats, delimiter=",")

        # save to csv file
        file1 = wkld_msg_stats_file+".csv"
        file2 = app_mpi_replay_stats_file+".csv"
        # file3 = app_io_stats_file+".csv"
        with open(file1, 'w') as outputfile:
            # outputfile.write(header)
            np.savetxt(outputfile, wkld_msg_stats, delimiter=",")        
        with open(file2, 'w') as outputfile:
            # outputfile.write(header)
            np.savetxt(outputfile, app_mpi_replay_stats, delimiter=",")  
        # with open(file3, 'w') as outputfile:
        #     # outputfile.write(header)
        #     np.savetxt(outputfile, app_io_stats, delimiter=",")  

def sep_app_traffic_stats(path, allocpath='./conf/alloc/', outputpath='./linkstats/'):

    # load allocation files
    alloc={}
    # alloc['cont']
    cont_alloc_file = os.path.join(allocpath, 'cont-1d-1056-mixwkld.conf')
    rand_alloc_file = os.path.join(allocpath, 'rand_node0-1d-1056-mixwkld.conf')

    alloc['cont'] = np.genfromtxt(cont_alloc_file, delimiter=" ", skip_header=0)
    alloc['rand'] = np.genfromtxt(rand_alloc_file, delimiter=" ", skip_header=0)

    for subdir in next(os.walk(path))[1]:
        parser = subdir.split("-")
        thisalloc = parser[0].split("_")[0]
        thisrotr = parser[1]
        thisappname = parser[2]

        if 'wkld' in thisappname:
            num_apps = 3
        else:
            num_apps = 1

        lp_output_folder = os.path.join(path, subdir)
        router_stats_file = os.path.join(lp_output_folder, 'dragonfly-link-stats')
        # Format <source_id> <source_type> <dest_id> < dest_type>  <link_type> <link_traffic> <link_saturation> <stalled_chunks>
        router_stats = np.genfromtxt(router_stats_file, delimiter=None, dtype=None, skip_header=2, names=['srcid', 'srctype', 'dstid', 'dsttype', 'linktype', 'linktraffic', 'linksat', 'stall'])

        for appid in range(num_apps):
            if 'wkld' in thisappname:
                routerlist = [ int(x/4) for x in alloc[thisalloc][appid]]
            else:
                routerlist = [ int(x/4) for x in alloc[thisalloc][apporder[thisappname]]]

            routerlist = list(set(routerlist))

            Lstats = []
            Gstats = []
            Allstats = []

            a=0
            for item in router_stats:
                if item['linktype']=="CN":
                    continue
                if item['srcid'] in routerlist:
                    Allstats.append([item['srcid'], item['dstid'], item['linktraffic'], item['linksat']])
                    if item['linktype']=="L":
                        Lstats.append([item['srcid'], item['dstid'], item['linktraffic'], item['linksat']])
                    elif item['linktype']=="G":
                        a += 1
                        Gstats.append([item['srcid'], item['dstid'], item['linktraffic'], item['linksat']])

            # print len(Gstats), a

            # get app name
            if 'wkld' in thisappname:
                appn = WKLD[thisappname][appid]+'-'+thisappname
            else:
                appn = thisappname+'-baseline'

            # write to files
            file1=os.path.join(outputpath, appn+'-'+thisalloc+'-'+thisrotr+"-link-stats.csv")
            file2=os.path.join(outputpath, appn+'-'+thisalloc+'-'+thisrotr+"-Llink-stats.csv")
            file3=os.path.join(outputpath, appn+'-'+thisalloc+'-'+thisrotr+"-Glink-stats.csv")
            with open(file1, 'w') as outputfile:
                np.savetxt(outputfile, Allstats, delimiter=",")        
            with open(file2, 'w') as outputfile:
                np.savetxt(outputfile, Lstats, delimiter=",")  
            with open(file3, 'w') as outputfile:
                np.savetxt(outputfile, Gstats, delimiter=",")  


if __name__ == "__main__":
    path = sys.argv[1]
    # num_apps = int(sys.argv[2])
    # sep_app_msg_stats(num_apps, path)
    sep_app_traffic_stats(path)
