/***************************************************************************
 *   Copyright (C) 2008-2010 by Walter Brisken                             *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
//===========================================================================
// SVN properties (DO NOT CHANGE)
//
// $Id$
// $HeadURL$
// $LastChangedRevision$
// $Author$
// $LastChangedDate$
//
//============================================================================

#include <stdlib.h>
#include <string.h>
#include "difxio/difx_write.h"

int writeDifxCalc(const DifxInput *D)
{
	FILE *out;

	if(!D)
	{
		return -1;
	}

	if(!D->job)
	{
		fprintf(stderr, "writeDifxCalc: job=0\n");

		return -1;
	}

	if(D->calcFile[0] == 0)
	{
		fprintf(stderr, "developer error: writeDifxCalc: D->calcFile is null\n");

		return -1;
	}

	out = fopen(D->calcFile, "w");
	if(!out)
	{
		fprintf(stderr, "Cannot open %s for write\n", D->calcFile);

		return -1;
	}

	writeDifxLineInt(out, "JOB ID", D->job->jobId);
	if(D->fracSecondStartTime > 0)
	{
		writeDifxLineDouble(out, "JOB START TIME", "%13.7f", D->job->jobStart);
		writeDifxLineDouble(out, "JOB STOP TIME", "%13.7f", D->job->jobStop);
	}
	else
	{
		writeDifxLineDouble(out, "JOB START TIME", "%13.7f", roundSeconds(D->job->jobStart));
		writeDifxLineDouble(out, "JOB STOP TIME", "%13.7f", roundSeconds(D->job->jobStop));
	}
	if(D->job->dutyCycle > 0.0)
	{
		writeDifxLineDouble(out, "DUTY CYCLE", "%5.3f", D->job->dutyCycle);
	}
	writeDifxLine(out, "OBSCODE", D->job->obsCode);
	writeDifxLine(out, "DIFX VERSION", D->job->difxVersion);
	writeDifxLineInt(out, "SUBJOB ID", D->job->subjobId);
	writeDifxLineInt(out, "SUBARRAY ID", D->job->subarrayId);
	if(D->fracSecondStartTime > 0)
	{
		writeDifxLineDouble(out, "START MJD", "%13.7f", truncSeconds(D->mjdStart));
		writeDifxDateLines(out, truncSeconds(D->mjdStart));
	}
	else
	{
		//round to nearest second - consistent with what is done in write_input
		writeDifxLineDouble(out, "START MJD", "%13.7f", roundSeconds(D->mjdStart));
		writeDifxDateLines(out, roundSeconds(D->mjdStart));
	}
	writeDifxLineInt(out, "SPECTRAL AVG", D->specAvg);
	writeDifxLine(out, "TAPER FUNCTION", D->job->taperFunction);
	writeDifxAntennaArray(out, D->nAntenna, D->antenna, 1, 1, 1, 0, 1);
        writeDifxSourceArray(out, D->nSource, D->source, 1, 1, 0);
	writeDifxScanArray(out, D->nScan, D->scan, D->config);
	writeDifxEOPArray(out, D->nEOP, D->eop);
	writeDifxSpacecraftArray(out, D->nSpacecraft, D->spacecraft);
	writeDifxLine(out, "IM FILENAME", D->imFile);

	fclose(out);

	return 0;
}
