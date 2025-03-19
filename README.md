# Reproducibility PADS 2025

This documents describes how to reproduce the results discussed in the paper:

"MFNetSim: A Multi-Fidelity Network Simulation Framework for Multi-Traffic Modeling of Dragonfly Systems"

Accepted to ACM SIGSIM PADS 2025

## Authors & Contacts

* Xin Wang <xwang823@uic.edu>
* Kevin A. Brown <kabrown@anl.gov>
* Robert B. Ross <rross@mcs.anl.gov>
* Christopher D. Carothers <chrisc@cs.rpi.edu>
* Zhiling Lan <zlan@uic.edu>


## Requirements

* x86_64 CPU
* 64GB of RAM
* Unix system with gcc/g++ toolchain

The hardware/software configuration used by the authors is Bebop at ALCF, with the following specifications:

* CPU: Intel(R) Xeon(R) CPU E5-2695 v4 @ 2.10GHz
* RAM: 125GB
* OS: Rocky Linux 8.10 (Green Obsidian)


## Dependencies

* For running tests: ```bash, gcc, g++, cmake, make, libtool, pkg-config, python2, openmpi/other mpi library, Flex, Bison ```
* For processing data and generating figures: ```bash, python2, pip2 ```


## Structure of the artifact

```
source_codes/
 |-- experiments/         /* experiments executable */
     |-- conf/            /* configuration files for experiment parameters                        */
     |-- ind/             /* store experiment outputs for baseline cases                          */
     |-- wkld/            /* store experiment outputs for mixed workload cases                    */
     |-- appstats/        /* store postprocessed application statistics from  experiment outputs  */
     |-- run_exp.sh       /* script to execute experiments                                        */
     |-- draw_plots.sh    /* script to process experiment outputs and plot figures in the paper   */
     |-- drawlinkstat.py  /* supplementary code for draw_plots.sh       */
     |-- draw.py          /* supplementary code for draw_plots.sh       */
     |-- sep_by_appid.py  /* supplementary code for draw_plots.sh       */     
 |-- mfnetsim/            /* source code for our proposed MFNetSim      */
 |-- swm-workloads/       /* source code for SWM skeleton applications  */
 |-- README.md            /* This file */
 |-- compile_script.sh    /* script for compiling and installing dependencies for MFNetSim */

```


## Article claims

The article has two major claims:

* C1: We develop a multi-fidelity modeling framework that enables simulation of multi-traffic simultaneously over the interconnect network, including inter-process communication and I/O traffics.
* C2: We conduct simulation studies of hybrid workloads composed of traditional HPC applications and emerging ML applications on a 1,056-node Dragonfly system with various configurations.


## Reproducing the results

Figure 10, 11, 12, 13 from the paper can be reproduced.
The mapping between claims, experiments, figures and tables are resumed in the following table.

| Claim  | Figures            | Experiment          |
|--------|--------------------|---------------------|
| C1, C2 | 10, 11, 12, 13     | run_exp, draw_plots |

To compile the program:
1. ```cd source_codes```
2. ```./compile_script.sh```

To run all experiments and process its results, type the following:

3. ```cd source_codes/experiments```
4. ```./run_exp.sh```

Wait all experiments to finish and process their results at once, type the following:

5. ```cd source_codes/experiments```
6. ```./draw_plots.sh```

The expected runtime of each experiment is detailed in the following table:

| Experiment           | Runtime |
|----------------------|---------|
| baseline cases       |  9.05h  |
| mixed workload cases |  9.89h  |
| **Total**            | 18.94h  |


Once all experiments have been run, you can find Figure 11 under linkfig/ folder, and Figure 10, 12, 13 under appfig/ folder.

