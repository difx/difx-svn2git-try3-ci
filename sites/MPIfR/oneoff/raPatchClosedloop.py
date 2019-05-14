#!/usr/bin/python
"""
Patch a DiFX/CALC .im file delay and uvw polynomials with RadioAstron closed-loop correction files.

Usage: raPatchClosedloop.py [-r <add dly rate s/s>] <dly_polys.txt> <uvw_polys.txt> <difxbasename1.im> [<difxbasename2.im> ...]

Input:
    dly_polys.txt     RadioAstron closed-loop delay polynomials
    uvw_polys.txt     RadioAstron closed-loop u,v,w polynomials
    difxbasename1.im  original DiFX/CALC .im file to patch

Output:
    difxbasename1.im.closedloop
"""
from datetime import datetime, timedelta
from calendar import timegm
import math, sys, time
import argparse

SCALE_DELAY = 1e6	# scaling to get from RA_C_COH.TXT units (secs) to .im units (usec)
SCALE_UVW = 1		# scaling to get from RA_C_COH_uvw.txt units (m?) to .im units (m)

parser = argparse.ArgumentParser(add_help=False, description='Patch a DiFX/CALC .im file delay and uvw polynomials with RadioAstron closed-loop correction files.')
parser.add_argument('-h', '--help', help='Help', action='store_true')
parser.add_argument('-r', '--drate', default=0.0, dest='ddlyrate', help='Residual delay rate in s/s to add to dly polynomial')
parser.add_argument('-t', '--dt', default=0.0, dest='dtshift', help='Time shift polys p(t) to p(t+dt) by dt seconds via change of coefficients')
parser.add_argument('-N', '--maxorder', default=12, dest='maxorder', help='Maximum poly order; set higher coeffs to zero if present')
parser.add_argument('files', nargs='*')

class PolyCoeffs:
	"""A single polynomial with coefficients"""

	source = ''
	Ncoeffs = 0
	dims = 0
	coeffs = []
	coeffscale = 1.0
	tstart = datetime.utcnow() 
	tstop = tstart
	interval = 0

	def getArgs(self, line):
		return line.split('=')[-1].strip()

	def raStrToTime(self, s):
		"""Parse 'dd/mm/yyyy HHhMMmSSs' into datetime"""
		# https://aboutsimon.com/blog/2013/06/06/Datetime-hell-Time-zone-aware-to-UNIX-timestamp.html
		gm = timegm( time.strptime(s + ' GMT', '%d/%m/%Y %Hh%Mm%Ss %Z') )
		d = datetime.utcfromtimestamp(gm)
		return d

	def __init__(self, details, coeffscale):
		"""Initialize coeffs using a list of strings stored in 'details'.
		The list of strings should have the following format, with 'source',
		'start', 'stop', and a varying number of P0..P<n> coefficient lines.
		Coefficient line k should contain one (1D) or more (e.g. 3D) coefficients
		for the k-th power terms (x^k * Px_k + y^k * Py_k + z^k * Pz_k + ...)

		source = 0716+714
		start = 19/10/2017 09h00m00s
		stop = 19/10/2017 09h01m00s
		P0 = -1.00975710299507e-001, -2.49717010234864e-001, 5.28185040398344e-001
		P1 = 9.18124737968820e-007, -4.59083189789325e-006, 1.99785028077570e-006
		P2 = 3.57024938682854e-012, 8.83446311517487e-012, -1.87750268902654e-011
		P3 = -7.78908442044021e-016, 1.48057049755645e-015, 8.27468673485702e-016
		P4 = 1.41735592450728e-017, -2.51428930101850e-017, -1.61417000499427e-017
		P5 = -9.33938621249164e-020, 1.57978134340036e-019, 1.12339170239300e-019
		"""
		self.Ncoeffs = len(details) - 3
		assert(self.Ncoeffs >= 1)
		self.source = self.getArgs(details[0])
		self.tstart = self.raStrToTime( self.getArgs(details[1]) )
		self.tstop = self.raStrToTime( self.getArgs(details[2]) )
		self.interval = (self.tstop - self.tstart).total_seconds()
		self.coeffs = []
		self.coeffscale = coeffscale
		for n in range(self.Ncoeffs):
			cstr = self.getArgs(details[3+n])
			c = [coeffscale*float(s) for s in cstr.split(',')]
			self.dims = len(c)
			self.coeffs.append(c)

	def add(self, corrections):
		'''Element-wise addition of values in list 'correction' to the coefficients'''
		for n in range(min(self.Ncoeffs,len(corrections))):
			self.coeffs[n] = [val + corrections[n] for val in self.coeffs[n]]

	def trunc(self, maxorder):
		'''Set to zero all coefficients that are present past 'maxorder' (0=const-only, 1=linear-only, etc).'''
		for n in range(self.Ncoeffs):
			if n > maxorder:
				self.coeffs[n] =  [0.0]*self.dims

	def binom(self, n):
		'''Binomial coeffs, computed by the multipliative formula'''
		n = int(n)
		coeff_k = [0]*(n+1)
		for k in range(n+1):
			c = 1.0
			for i in range(1, k+1):
				c *= float((n+1-i))/float(i)
			coeff_k[k] = int(c)
		return coeff_k

	def timeshift(self, dt):
		'''Time shift polynomials p(t) to p(t+dt) via change of coefficients'''
		# Ideally would use "Taylor shift" e.g. https://math.stackexchange.com/questions/694565/polynomial-shift
		# but since we don't need performance, can use polynomial expansion at t+dt and update of coefficients.
		#
		# Illustration of the method:
		#
		# Shift p(t)    = a + b*t + c*t^2 + ...
		# gives p(t+dt) = <below>
		#
		# order 0:  p(t+dt) = a
		# order 1:  p(t+dt) = a + (b*t + b*dt) = (a + b*dt) + b*t
		#           a' = a + b*dt
		#           b' = b
		# order 2:  p(t+dt) = a + (b*t + b*dt) + (c*t^2 + c*2*dt*t + c*dt^2)
		#           a' = a + b*dt + c*dt^2
		#           b' = b + c*2*dt
		#           c' = c
		# order 3:  p(t+dt) = a + (b*t + b*dt) + (c*t^2 + c*2*dt*t + c*dt^2) + (d*dt^3 + d*3*dt^2*t + d*3*dt*t^2 + d*t^3)
		#           a' = a + b*dt + c*dt^2 + d*dt^3
		#           b' = b + c*2*dt + d*3*dt^2
		#           c' = c + d*3*dt
		#           d' = d
		# order 4:  add e*dt^4  + e*4*dt^3*t + e*6*dt^2*t^2 + e*4*dt*t^3 + e*t^4 
		#           a' = a + b*dt + c*dt^2 + d*dt^3 + e*dt^4
		#           b' = b + c*2*dt + d*3*dt^2 + e*4*dt^3
		#           c' = c + d*3*dt + e*6*dt^2
		#           d' = d + e*4*dt
		#           e' = e
		# order 5:  add f*dt^5 + f*5*dt^4*t + f*10*dt^3*t^2 + f*10*dt^2*t^3 + f*5*dt*t^4 + f*t^5
		#           a' = a + b*dt + c*dt^2 + d*dt^3 + e*dt^4 + f*dt^5
		#           b' = b + c*2*dt + d*3*dt^2 + e*4*dt^3 + f*5*dt^4
		#           c' = c + d*3*dt + e*6*dt^2 + f*10*dt^3
		#           d' = d + e*4*dt + f*10*dt^2
		#           e' = e + f*5*dt
		#           f' = f
		# --> diagonals of Pascal's triangle

		if not dt:
			return

		# Precompute Pascal's triangle
		binom_nk = [[]] * (self.Ncoeffs+1)
		for n in range(self.Ncoeffs+1):
			binom_nk[n] = self.binom(n)
		# print (self.Ncoeffs, binom_nk)

		# Precompute dt^n for n=0..Ncoeff
		dt = dt * self.coeffscale
		dtpow = [math.pow(dt,n) for n in range(self.Ncoeffs+1)]
		print dt, dtpow

		# Shift the polynomial. Need to shift each poly/dimension (dly: 1D, uvw: 3D).
		for d in range(self.dims):
			oldcoeffs = [self.coeffs[n][d] for n in range(self.Ncoeffs)]
			newcoeffs = [0.0] * self.Ncoeffs
			for n in range(self.Ncoeffs):
				print ('c[%d] = 0 ' % (n)),
				for k in range(n, self.Ncoeffs):
					newcoeffs[n] += oldcoeffs[k]*binom_nk[k][n]*dtpow[k-n]
					print (' + old[%d]*%d*dt^%d' % (k,binom_nk[k][n],k-n)), 
				print ('')
			print ('old:', oldcoeffs)
			print ('new:', newcoeffs)
			print ('')
			for n in range(self.Ncoeffs):
				self.coeffs[n][d] = newcoeffs[n]

class PolySet:
	"""Storage of a set of polynomials"""

	piecewisePolys = []
	startSec = 1e99
	dims = 0

	def __init__(self, filename, coeffscale=1):
		"""
		Create object and load a set of polynomials and their coefficients from a file.
		"""
		self.piecewisePolys = []
		with open(filename) as f:
			lines = f.readlines()
			lines = [l.strip() for l in lines]
		if len(lines) < 1:
			print ('Error: could not read %s' % (filename))
			return
		if not 'RASTRON' in lines[0]:
			print ('Unexpected delay poly file content')
			return
		N = int(lines[1].split('=')[1])
		lnr = 3
		while lnr < len(lines):
			assert('source' in lines[lnr])
			polyDefinition = lines[lnr:(lnr+3+N)]
			self.piecewisePolys.append( PolyCoeffs(polyDefinition,coeffscale) )
			self.dims = self.piecewisePolys[-1].dims
			self.startSec = min(self.startSec, self.piecewisePolys[-1].tstart.second)
			lnr += 3 + N

	def __len__(self):
		return len(self.piecewisePolys)

	def datetimeFromMJDSec(self,MJD,sec):
		mjd_t0 = datetime(1858,11,17,0,0,0,0) # MJD 0 = 17 November 1858 at 00:00 UTC
		T = mjd_t0 + timedelta(days=MJD) + timedelta(seconds=sec)
		return T

	def lookupPolyFor(self,MJD,sec):
		"""
		Lookup up poly that was start time identical to the given MJD and second-of-day
		"""
		tlookup = self.datetimeFromMJDSec(MJD,sec)
		for poly in self.piecewisePolys:
			if poly.tstart == tlookup:
				return poly
			if poly.tstart < tlookup and poly.tstop > tlookup:
				dt = (tlookup - poly.tstart).total_seconds()
				print ('Error: Time %d MJD %d sec (%s) not at start but rather %d seconds into RA poly.' % (MJD,sec,str(tlookup),dt))
				print ('       Poly time-shift not supported yet!')
				return None
		return None

	def add(self, corrections):
		'''Element-wise addition of values in list 'correction' to coeffs of all polynomials'''
		for poly in self.piecewisePolys:
			poly.add(corrections)

	def trunc(self, maxorder):
		'''Set to zero all coefficients that are present past 'maxorder' (0=const-only, 1=linear-only, etc).'''
		for poly in self.piecewisePolys:
			poly.trunc(int(maxorder))

	def timeshift(self, dt):
		'''Time shift polynomials p(t) to p(t+dt) via change of coefficients'''
		for poly in self.piecewisePolys:
			poly.timeshift(float(dt))


def getKey(s):
	"""Return key part of DiFX .im file type "key: option" string"""
	return s.split(':')[0].strip()


def getOpt(s):
	"""Return option part of DiFX .im file type "key: option" string"""
	return s.split(':')[1].strip()


def getKeyOpt(s):
	return (getKey(s),getOpt(s))


def imGetLineWith(lines,nstart,keys):
	"""
	Looks for the next line containing all key(s).
	Returns (linenr,linecontent).
	"""
	if not(type(keys) is list):
		keys = [keys]
	n = nstart
	while n < len(lines):
		if all([k in lines[n] for k in keys]):
			return (n,lines[n])
		n += 1
	return (len(lines),'')


def imDetectNextScanblock(lines,nstart):
	"""
	Look for next SCAN block in IM file lines, starting from line nr 'nstart'.
	Returns (istart,istop,srcname,mjd,sec) of the next scan block, or None.
	"""
	n,srcline = imGetLineWith(lines,nstart,['SCAN','POINTING SRC'])
	if (n + 6) >= len(lines):
		return None

	n,mjdline = imGetLineWith(lines,n,['POLY','MJD'])
	iscanstart = n
	n,secline = imGetLineWith(lines,n,['POLY','SEC'])

	srcname = getOpt(srcline)
	mjd = int(getOpt(mjdline))
	sec = int(getOpt(secline))

	iscanstop,tmp = imGetLineWith(lines,n,['SCAN','POINTING SRC'])
	return (iscanstart,iscanstop,srcname,mjd,sec)


def imDetectNextScanpolyblock(lines,nstart,nstop):
	"""
	Looks for next "SCAN <n> POLY <m>" block in file lines, starting from line nr 'nstart'.
	Returns (istart,istop,mjd,sec) of the next scan poly block, or None.
	"""
	mjd, sec = 0, 0
	n,mjdline = imGetLineWith(lines,nstart,['POLY','MJD'])
	if n >= nstop:
		return None
	n,secline = imGetLineWith(lines,n,['POLY','SEC'])
	mjd = int(getOpt(mjdline))
	sec = int(getOpt(secline))

	n += 1
	ipolystart = n
	while n < len(lines) and n < nstop:
		if 'SCAN' in lines[n] and ('POINTING SRC' in lines[n] or 'POLY' in lines[n]):
			break
		n += 1
	ipolystop = n
	blk = (ipolystart,ipolystop,mjd,sec)
	return blk


def imSumPolyCoeffs(telescope_id,lines,polystart,polystop,dpoly,uvwpoly,sign=-1):
	N_updated = 0
	map_tag_to_poly = {
		# line identifier                      storage   axis   type     linecount
		('ANT %d DELAY (us)' % telescope_id): [dpoly,    0,     'T',     0],
		('ANT %d U (m)' % telescope_id):      [uvwpoly,  0,     'uvw',   0],
		('ANT %d V (m)' % telescope_id):      [uvwpoly,  1,     'uvw',   0],
		('ANT %d W (m)' % telescope_id):      [uvwpoly,  2,     'uvw',   0]
	}

	for n in range(polystart,polystop):
		for tag in map_tag_to_poly:
			if tag in lines[n]:

				P, col, type, linecount = map_tag_to_poly[tag]
				map_tag_to_poly[tag][-1] += 1

				C = [ pp[col] for pp in P.coeffs ]
				key,oldcoeffs = getKeyOpt(lines[n])
				oldcoeffs = [float(v) for v in oldcoeffs.split('\t')]
				newcoeffs = list(oldcoeffs)

				# Element wise summation of correction coeffs onto polynomial
				N = min(len(newcoeffs), len(C)) # truncate to either poly
				for k in range(N):
					# Add?
					# newcoeffs[k] += sign*C[k]

					# Just copy?
					# newcoeffs[k] = sign*C[k]

					if type=='uvw':
						newcoeffs[k] = C[k]
					if type=='T':
						# Add zeroth order coarse dlys, copy higher-order from ASC poly
						if k==0:
							# newcoeffs[0] = sign*C[0] + oldcoeffs[0]
							# newcoeffs[0] = sign*C[0] - oldcoeffs[0]
							newcoeffs[0] = oldcoeffs[0]
							# newcoeffs[0] = sign*C[0]
						else:
							newcoeffs[k] = sign*C[k]

				newcoeffs_str = ' '.join(['%.16e\t ' % v for v in newcoeffs])
				newline = '%s:  %s' % (key.strip(),newcoeffs_str)

				# print ('------------------------------')
				# print (n)
				# print (C)
				# print (lines[n])
				# print (newline)
				# print ('------------------------------\n')

				# print (n, lines[n], C, newline)
				# print (lines[n], newline)

				lines[n] = newline
				N_updated += 1

	# print (map_tag_to_poly)

	return N_updated


def patchImFile(basename, dlypolys, uvwpolys, antname='GT'):
	if basename.endswith(('.difx','input','.calc','.im')):
		basename = basename[:basename.rfind('.')]
	imname = basename + '.im'
	imoutname = basename + '.im.closedloop'

	lines = []
	with open(imname) as f:
		lines = f.readlines()
		lines = [l.strip() for l in lines]
	if len(lines)<16:
		print ('Error: problem loading a valid %s!' % (imname))
		return False

	print ("\n%s\n" % (imname))

	# Find start time, make sure no time-shift is necessary
	im_start_sec = -1
	for line in lines:
		if 'START SECOND' in line:
			im_start_sec = float(line.split(':')[-1])
			break

	# Find telescope, poly order, poly interval
	telescope_id = None
	im_poly_order = 0
	im_poly_interval_s = 0
	antname = antname.upper()
	for line in lines:
		# 'TELESCOPE 0 NAME:   GT'
		if 'TELESCOPE' in line and 'NAME' in line:
			[key,val] = getKeyOpt(line)
			if val.strip().upper() == antname:
				telescope_id = int(key.split()[1])
				break
		# 'POLYNOMIAL ORDER:   5'
		if 'POLYNOMIAL ORDER' in line:
			im_poly_order = int(getOpt(line))
		# 'INTERVAL (SECS):    120'
		if 'INTERVAL (SECS)' in line:
			im_poly_interval_s = int(getOpt(line))

	# Consistency check IM <-> RA coeffs
	is_polyorder_mismatched = [poly.Ncoeffs > (im_poly_order+1) for poly in dlypolys.piecewisePolys + uvwpolys.piecewisePolys]
	is_polystartsec_mismatched = (im_start_sec % im_poly_interval_s) != dlypolys.startSec
	is_polyinterval_mismatched = [poly.interval < im_poly_interval_s for poly in dlypolys.piecewisePolys + uvwpolys.piecewisePolys]
	if telescope_id == None:
		print ('Error: could not find telescope %s in %s' % (antname,imname))
		return False
	if any(is_polyorder_mismatched):
		print ('Warning: mismatch in polynomial order of .im file (%d) and closed-loop file (%d).' % (im_poly_order, poly.Ncoeffs-1))
	if any(is_polyinterval_mismatched):
		print ('Error: too long polynomial validity interval in .im file (%d sec) for appyling closed-loop polys (%d sec).' % (im_poly_interval_s, poly.interval))
		return False
	if is_polystartsec_mismatched:
		print ('Error: mismatch between .im base start time at second %d and poly start at second %d!' % (im_start_sec, dlypolys.startSec))
		print ('Currently no support for polynomial time-shifting. Please edit these files and re-run calcif3 and patching:')
		print ('   .calc  file set START SECOND to 0 or %d sec multiple' % (im_poly_interval_s))
		print ('   .input file adjust START SECONDS to fall on 0 sec or %d sec boundary' % (im_poly_interval_s))
		print ('or alternatively edit')
		print ('   .vex   file adjust scan start=<start> to fall on 0 sec or %d sec boundary' % (im_poly_interval_s))
		print ('          and extend scan length by %d seconds' % (im_start_sec % im_poly_interval_s))
		return False

	# Patch all relevant IM file lines
	nupdated_total = 0
	n = 0
	print ("Applying closed-loop coefficients to telescope %s with id %d" % (antname,telescope_id))
	while n < len(lines):

		# Find next 'SCAN' block
		blk = imDetectNextScanblock(lines,n)
		if blk == None:
			break
		(blkstart,blkstop,srcname,mjd,sec) = blk

		# Check all 'SCAN <n> POLY <m>' sections of the block
		pstart = blkstart
		while True:

			# Get next 'POLY <m>'
			blkpoly = imDetectNextScanpolyblock(lines,pstart,blkstop)
			if blkpoly == None:
				break
			(polystart,polystop,mjd,sec) = blkpoly

			# Get the matching Closed Loop polynomial coeffs sets
			dp = dlypolys.lookupPolyFor(mjd,sec)
			uvwp = uvwpolys.lookupPolyFor(mjd,sec)
			if dp == None or uvwp == None:
				T = dlypolys.datetimeFromMJDSec(mjd,sec)
				print('Warning: no suitable poly found in coeffs file to match MJD %d sec %d (%s)!' % (mjd,sec,str(T)))
				pstart = polystop
				continue
			if dp.source != uvwp.source:
				print("Error: RA delay poly source '%s' does not match UVW poly source '%s'!" % (dp.source,uvwp.source))
			if dp.source != srcname:
				print("Error: IM source '%s' does not match poly coeff set source '%s'!" % (srcname,dp.source))
				return False

			# Apply the coeffs
			nupdated = imSumPolyCoeffs(telescope_id,lines,polystart,polystop,dp,uvwp)
			nupdated_total += nupdated

			# Next poly segment
			pstart = polystop

		# Next poly block
		#n = blkstop + 1
		n = blkstop

	# Write the new IM file
	f = open(imoutname, 'w')
	for line in lines:
		f.write(line + '\n')

	print ("Wrote new file '%s' with %d updated coefficient lines." % (imoutname,nupdated_total))
	print ("Success: processed %s\n" % (imname))
	return True


if __name__ == "__main__":

	for i, arg in enumerate(sys.argv):
		# workaround from https://stackoverflow.com/questions/9025204/python-argparse-issue-with-optional-arguments-which-are-negative-numbers
		if (arg[0] == '-') and arg[1].isdigit(): sys.argv[i] = ' ' + arg

	args = parser.parse_args(sys.argv[1:])
	if args.help or len(args.files) < 3:
		print(__doc__)
		sys.exit(-1)

	dly = PolySet(args.files[0], coeffscale=SCALE_DELAY)
	if len(dly) < 1:
		print ("Error: could not load delay polynomials from '%s'" % (args.files[0]))
		sys.exit(-1)

	uvw = PolySet(args.files[1], coeffscale=SCALE_UVW)
	if len(uvw) < 1:
		print ("Error: could not load u,v,w polynomials from '%s'" % (args.files[1]))
		sys.exit(-1)

	if dly.dims != 1 or uvw.dims != 3:
		print ("Error: coeff file poly dimensions mismatch! Expected dim. 1 for 1st file '%s' (%d), dim. 3 for 2nd file '%s' (%d)" % (args.files[0], dly.dims, args.files[1], uvw.dims))
		sys.exit(-1)

	dly.add([0.0, float(args.ddlyrate)])
	dly.trunc(args.maxorder)
	dly.timeshift(args.dtshift)
	uvw.trunc(args.maxorder)
	# uvw.timeshift(args.dtshift)

	for difxf in args.files[2:]:
		ok = patchImFile(difxf, dly, uvw)
		if not ok:
			print ("Error: failed to patch %s" % (difxf))
