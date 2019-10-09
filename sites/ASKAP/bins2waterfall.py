#!/usr/bin/env python

# NOTE: to use on ozstar with a local copy of CASA, you need to module load the following:
# module load numpy/1.16.3-python-2.7.14
# module load matplotlib/2.2.2-python-2.7.14

import numpy as np
import matplotlib.pyplot as plt
import os, sys,argparse

parser = argparse.ArgumentParser()
parser.add_argument("-n", "--nbins", type=int, default=None, help="Number of image files from which to extract the spectrum; note: zero-indexed, so nbins=11 means bin00 to bin10")
parser.add_argument("-c", "--nchan", type=int, default=83, help="Number of channel slices in each cube image; note: zero-indexed")
parser.add_argument("-s", "--src", type=str, default=None, help="Source name to be used for the spectra text file prefix")
parser.add_argument("--rms", help="Set if noise estimation is required", action="store_true")
parser.add_argument("-p", "--prefix", type=str, default = "", help="Prefix for bin directory")

args = parser.parse_args()

print args

if len(sys.argv) < 2:
    parser.print_usage()
    sys.exit()

if args.nbins is None:
    parser.error("You must specify the number of images you're processing")

if args.nchan is None:
    parser.error("You must specify the number of slices in the cube image")

if args.src is None:
    parser.error("You must specify an output spectra file name prefix")

nbins = args.nbins
nchan = args.nchan
src = args.src
prefix = args.prefix

os.system("mkdir {0}bins2waterlogs".format(prefix))

for stokes in ["I","Q","U","V","XX","YY"]:
    dynspec = np.zeros(nbins*nchan).reshape(nbins,nchan)
    
    for i in range(nbins):
        inputimage = "{0}bin{1:02g}/TARGET.cube.{2}.image".format(prefix, i, stokes)
        runfile = "waterfall.py"
        print inputimage

        # Get the max flux of the source from source images
        output = open(runfile, "w")
        for chan in range(nchan):
            output.write('imstat(imagename="{0}",box="64,63,65,64",chans="{1:d}",logfile="waterfall.bin{2:02d}.chan{3:03d}.log")\n'.format(inputimage, chan, i, chan))
        output.close()
        os.system("casa --nologger -c {0}".format(runfile))
        for chan in range(nchan):
            logfile = "waterfall.bin{0:02d}.chan{1:03d}.log".format(i, chan)
            loglines = open(logfile).readlines()
            for line in loglines:
                if "maximum value [max]:" in line:
                    dynspec[i][chan] = float(line.split()[-2]) # unit = Jy/beam
                    break
        os.system("rm -f waterfall.*.log")
        os.system("mv casa*.log {0}bins2waterlogs".format(prefix))

    np.savetxt("{0}-imageplane-dynspectrum.stokes{1}.txt".format(src, stokes), dynspec)

if args.rms:
    for stokes in ["I","Q","U","V","XX","YY"]:
        rms = np.zeros(nbins*nchan).reshape(nbins,nchan)
    
        for i in range(nbins):
            noiseimage = "{0}bin{1:02g}/OFFSOURCE.cube.{2}.image".format(prefix, i, stokes)
            getnoise = "get_rms.py"
            print noiseimage

            # Get RMS from noise estimation images (using 75% of 512x512 image)
            noiseout = open(getnoise, "w")
            for chan in range(nchan):
                noiseout.write('imstat(imagename="{0}",box="64,64,448,448",chans="{1:d}",logfile="noise.bin{2:02d}.chan{3:03d}.log")\n'.format(noiseimage, chan, i, chan))
            noiseout.close()
            os.system("casa --nologger -c {0}".format(getnoise))
            for chan in range(nchan):
                logfile = "noise.bin{0:02d}.chan{1:03d}.log".format(i, chan)
                loglines = open(logfile).readlines()
                for line in loglines:
                    if "Root mean square [rms]:" in line:
                        rms[i][chan] = float(line.split()[-2]) # unit = Jy/beam
                        break
            os.system("rm -f noise.*.log")
            os.system("mv casa*.log {0}bins2waterlogs".format(prefix))

        np.savetxt("{0}-imageplane-rms.stokes{1}.txt".format(src, stokes), rms)