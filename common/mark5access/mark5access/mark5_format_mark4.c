/***************************************************************************
 *   Copyright (C) 2006, 2007 by Walter Brisken                            *
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "mark5access/mark5_stream.h"

#define PAYLOADSIZE 20000
#define VALIDSTART 96
#define VALIDEND 19936

/* the high mag value for 2-bit reconstruction */
static const float HiMag = 3.3359;

static float lut1bit[256][8];  /* For all 1-bit modes */
static float lut2bit1[256][4]; /* fanout 1 @ 8/16t, fanout 4 @ 32/64t ! */
static float lut2bit2[256][4]; /* fanout 2 @ 8/16t, fanout 1 @ 32/64t   */
static float lut2bit3[256][4]; /* fanout 4 @ 8/16t, fanout 2 @ 32/64t   */
static float zeros[8];         /* initial value triggers initialization */

/* ! 2bit/fanout4 use the following in decoding 32 and 64 track data: */

#ifdef WORDS_BIGENDIAN

#define reorder32(x) ((((x) & 0x55AA55AAUL)) | \
                      (((x) & 0xAA00AA00UL) >> 9) | \
		      (((x) & 0x00550055UL) << 9))
#define reorder64(x) ((((x) & 0x55AA55AA55AA55AAULL)) | \
                      (((x) & 0xAA00AA00AA00AA00ULL) >> 9) | \
		      (((x) & 0x0055005500550055ULL) << 9))

#else

#define reorder32(x) ((((x) & 0xAA55AA55UL)) | \
                      (((x) & 0x55005500UL) >> 7) | \
		      (((x) & 0x00AA00AAUL) << 7))
#define reorder64(x) ((((x) & 0xAA55AA55AA55AA55ULL)) | \
                      (((x) & 0x5500550055005500ULL) >> 7) | \
		      (((x) & 0x00AA00AA00AA00AAULL) << 7))

#endif

struct mark5_format_mark4
{
	int ntrack;
	int fanout;
	int decade;	/* for proper date decoding.  should be 0, 10, 20... */
};

int countbits(uint8_t v);

static void initluts()
{
	int b, i, s, m, l;
	const float lut2level[2] = {1.0, -1.0};
	const float lut4level[4] = {-HiMag, 1.0, -1.0, HiMag};

	for(i = 0; i < 8; i++)
	{
		zeros[i] = 0.0;
	}

	for(b = 0; b < 256; b++)
	{
		/* lut1bit */
		for(i = 0; i < 8; i++)
		{
			l = (b>>i)&1;
			lut1bit[b][i] = lut2level[l];
		}

		/* lut2bit1 */
		for(i = 0; i < 4; i++)
		{
			s = i*2;	/* 0, 2, 4, 6 */
			m = s+1;	/* 1, 3, 5, 7 */
			l = ((b>>s)&1) + (((b>>m)&1)<<1);
			lut2bit1[b][i] = lut4level[l];
		}

		/* lut2bit2 */
		for(i = 0; i < 4; i++)
		{
			s = i+(i/2)*2;	/* 0, 1, 4, 5 */
			m = s+2;	/* 2, 3, 6, 7 */
			l = ((b>>s)&1) + (((b>>m)&1)<<1);
			lut2bit2[b][i] = lut4level[l];
		}

		/* lut2bit3 */
		for(i = 0; i < 4; i++)
		{
			s = i;		/* 0, 1, 2, 3 */
			m = s+4;	/* 4, 5, 6, 7 */
			l = ((b>>s)&1) + (((b>>m)&1)<<1);
			lut2bit3[b][i] = lut4level[l];
		}
	}
}

/* Look for the first occurance (lowest offset >= 0) of the following pattern:
 *
 * 32*tracks bits set at offset bytes
 * 32*tracks bits set at offset+2500*tracks bytes
 * 1*tracks bits unset at offset+2499*tracks bytes
 *
 * With up to tol bits not set correctly
 *
 * return offset;
 */
static int findfirstframe(const uint8_t *data, int bytes, int tracks, int tol)
{
	int offset;
	int wrong = 0;
	int i, a, b;

	if(bytes < 2600*tracks)
	{
		return -1;
	}
	
	bytes -= 2600*tracks;

	b = tracks*2500;
	a = b - tracks/8;

	for(i = 0; i < 4*tracks; i++)
	{
		wrong += countbits(~data[i]);
		wrong += countbits(~data[i+b]);
	}
	for(i = 0; i < tracks/8; i++)
	{
		wrong += countbits(data[i+a]);
	}

	for(offset = 0; offset < bytes; offset++)
	{
		if(wrong < tol)
		{
			return offset;
		}
		wrong -= countbits(~data[offset]);
		wrong += countbits(~data[offset+4*tracks]);
		wrong -= countbits(~data[offset+b]);
		wrong += countbits(~data[offset+b+4*tracks]);
		wrong -= countbits(data[offset+a]);
		wrong += countbits(data[offset+a+tracks/8]);
	}

	return -1;
}

/* look at encoded nibbles.  Count bits in each track, assume set if
 * more than half are
 */
static void extractnibbles(const uint8_t *data, int ntracks, int numnibbles, 
	char *nibbles)
{
	int i, j, b, n, c;

	n = ntracks/8;

	for(i = 0; i < numnibbles; i++)
	{
		nibbles[i] = 0;
		for(b = 0; b < 4; b++)
		{
			c = 0;
			for(j = 0; j < n; j++)
			{
				c += countbits(data[n*(4*i+3-b)+j]);
			}
			nibbles[i] += (c > n/2) ? (1 << b) : 0;
		}
	}
}

static int mark5_format_mark4_frame_time(const struct mark5_stream *ms, 
	int *mjd, int *sec, int *ns)
{
	char nibs[13];
	struct mark5_format_mark4 *v;
	const int lastdig[] = {0, 1250000, 2500000, 3750000, 0, 5000000,
				  6250000, 7500000, 8750000, 0, 0,0,0,0,0,0};

	if(!ms)
	{
		return -1;
	}
	v = (struct mark5_format_mark4 *)(ms->formatdata);

	extractnibbles(ms->frame + 4*v->ntrack, v->ntrack, 13, nibs);
	nibs[0] += v->decade;

	if(mjd)
	{
		*mjd = 51544 + 365*nibs[0] + nibs[1]*100
			+ nibs[2]*10 + nibs[3] + (int)(nibs[0]/4);
	}
	if(sec) 
	{
		*sec = nibs[4]*36000 + nibs[5]*3600 + nibs[6]*600 
			+ nibs[7]*60 + nibs[8]*10 + nibs[9];
	}
	if(ns)
	{
		*ns = nibs[10]*100000000 + nibs[11]*10000000 
			+ lastdig[(unsigned int)(nibs[12])];
	}

	return 0;
}

static int mark5_format_mark4_fixmjd(struct mark5_stream *ms, int refmjd)
{
	struct mark5_format_mark4 *v;
	char nibs[4];
	int decade;
	
	if(!ms)
	{
		return -1;
	}
	
	decade = (refmjd - ms->mjd + 1826)/3652.4;
	decade *= 10;

	if(decade > 0)
	{
		v = (struct mark5_format_mark4 *)(ms->formatdata);
		v->decade = decade;

		extractnibbles(ms->frame + 4*v->ntrack, v->ntrack, 4, nibs);
		nibs[0] += v->decade;

		ms->mjd = 51544 + 365*nibs[0] + nibs[1]*100
			+ nibs[2]*10 + nibs[3] + (int)(nibs[0]/4);

		return 1;
	}

	return 0;
}

/*********************** data unpack routines **********************/


static int mark4_decode_1bit_8track_fanout1(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp;
	int o, i;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp = zeros;
			nblank++;
		}
		else
		{
			fp = lut1bit[buf[i]];
		}
		i++;

		data[0][o] = fp[0];
		data[1][o] = fp[1];
		data[2][o] = fp[2];
		data[3][o] = fp[3];
		data[4][o] = fp[4];
		data[5][o] = fp[5];
		data[6][o] = fp[6];
		data[7][o] = fp[7];

		if(i >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
		}
	}

	ms->readposition = i;

	return nsamp - nblank;
}

static int mark4_decode_1bit_8track_fanout2(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp;
	int o, i;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp = zeros;
			nblank++;
		}
		else
		{
			fp = lut1bit[buf[i]];
		}
		i++;

		data[0][o] = fp[0];
		data[1][o] = fp[2];
		data[2][o] = fp[4];
		data[3][o] = fp[6];
		o++;
		data[0][o] = fp[1];
		data[1][o] = fp[3];
		data[2][o] = fp[5];
		data[3][o] = fp[7];

		if(i >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 2*nblank;
}

static int mark4_decode_1bit_8track_fanout4(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp;
	int o, i;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp = zeros;
			nblank++;
		}
		else
		{
			fp = lut1bit[buf[i]];
		}
		i++;

		data[0][o] = fp[0];
		data[1][o] = fp[4];
		o++;
		data[0][o] = fp[1];
		data[1][o] = fp[5];
		o++;
		data[0][o] = fp[2];
		data[1][o] = fp[6];
		o++;
		data[0][o] = fp[3];
		data[1][o] = fp[7];

		if(i >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 4*nblank;
}

static int mark4_decode_1bit_16track_fanout1(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/2;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = zeros;
			nblank++;
			i += 2;
		}
		else
		{
			fp0 = lut1bit[buf[i]];
			i++;
			fp1 = lut1bit[buf[i]];
			i++;
		}
		m++;

		data[0][o]  = fp0[0];
		data[1][o]  = fp0[1];
		data[2][o]  = fp0[2];
		data[3][o]  = fp0[3];
		data[4][o]  = fp0[4];
		data[5][o]  = fp0[5];
		data[6][o]  = fp0[6];
		data[7][o]  = fp0[7];
		data[8][o]  = fp1[0];
		data[9][o]  = fp1[1];
		data[10][o] = fp1[2];
		data[11][o] = fp1[3];
		data[12][o] = fp1[4];
		data[13][o] = fp1[5];
		data[14][o] = fp1[6];
		data[15][o] = fp1[7];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - nblank;
}

static int mark4_decode_1bit_16track_fanout2(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/2;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = zeros;
			nblank++;
			i += 2;
		}
		else
		{
			fp0 = lut1bit[buf[i]];
			i++;
			fp1 = lut1bit[buf[i]];
			i++;
		}
		m++;
		
		data[0][o] = fp0[0];
		data[1][o] = fp0[2];
		data[2][o] = fp0[4];
		data[3][o] = fp0[6];
		data[4][o] = fp1[0];
		data[5][o] = fp1[2];
		data[6][o] = fp1[4];
		data[7][o] = fp1[6];
		o++;
		data[0][o] = fp0[1];
		data[1][o] = fp0[3];
		data[2][o] = fp0[5];
		data[3][o] = fp0[7];
		data[4][o] = fp1[1];
		data[5][o] = fp1[3];
		data[6][o] = fp1[5];
		data[7][o] = fp1[7];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 2*nblank;
}

static int mark4_decode_1bit_16track_fanout4(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/2;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = zeros;
			nblank++;
			i += 2;
		}
		else
		{
			fp0 = lut1bit[buf[i]];
			i++;
			fp1 = lut1bit[buf[i]];
			i++;
		}
		m++;

		data[0][o] = fp0[0];
		data[1][o] = fp0[4];
		data[2][o] = fp1[0];
		data[3][o] = fp1[4];
		o++;
		data[0][o] = fp0[1];
		data[1][o] = fp0[5];
		data[2][o] = fp1[1];
		data[3][o] = fp1[5];
		o++;
		data[0][o] = fp0[2];
		data[1][o] = fp0[6];
		data[2][o] = fp1[2];
		data[3][o] = fp1[6];
		o++;
		data[0][o] = fp0[3];
		data[1][o] = fp0[7];
		data[2][o] = fp1[3];
		data[3][o] = fp1[7];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 4*nblank;
}

static int mark4_decode_1bit_32track_fanout1(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/4;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			nblank++;
			i += 4;
		}
		else
		{
			fp0 = lut1bit[buf[i]];
			i++;
			fp1 = lut1bit[buf[i]];
			i++;
			fp2 = lut1bit[buf[i]];
			i++;
			fp3 = lut1bit[buf[i]];
			i++;
		}
		m++;

		data[0][o]  = fp0[0];
		data[1][o]  = fp0[2];
		data[2][o]  = fp0[4];
		data[3][o]  = fp0[6];
		data[4][o]  = fp1[0];
		data[5][o]  = fp1[2];
		data[6][o]  = fp1[4];
		data[7][o]  = fp1[6];
		data[8][o]  = fp2[0];
		data[9][o]  = fp2[2];
		data[10][o] = fp2[4];
		data[11][o] = fp2[6];
		data[12][o] = fp3[0];
		data[13][o] = fp3[2];
		data[14][o] = fp3[4];
		data[15][o] = fp3[6];
		data[16][o] = fp0[1];
		data[17][o] = fp0[3];
		data[18][o] = fp0[5];
		data[19][o] = fp0[7];
		data[20][o] = fp1[1];
		data[21][o] = fp1[3];
		data[22][o] = fp1[5];
		data[23][o] = fp1[7];
		data[24][o] = fp2[1];
		data[25][o] = fp2[3];
		data[26][o] = fp2[5];
		data[27][o] = fp2[7];
		data[28][o] = fp3[1];
		data[29][o] = fp3[3];
		data[30][o] = fp3[5];
		data[31][o] = fp3[7];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - nblank;
}

static int mark4_decode_1bit_32track_fanout2(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/4;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			nblank++;
			i += 4;
		}
		else
		{
			fp0 = lut1bit[buf[i]];
			i++;
			fp1 = lut1bit[buf[i]];
			i++;
			fp2 = lut1bit[buf[i]];
			i++;
			fp3 = lut1bit[buf[i]];
			i++;
		}
		m++;

		data[0][o]  = fp0[0];
		data[1][o]  = fp0[4];
		data[2][o]  = fp1[0];
		data[3][o]  = fp1[4];
		data[4][o]  = fp2[0];
		data[5][o]  = fp2[4];
		data[6][o]  = fp3[0];
		data[7][o]  = fp3[4];
		data[8][o]  = fp0[1];
		data[9][o]  = fp0[5];
		data[10][o] = fp1[1];
		data[11][o] = fp1[5];
		data[12][o] = fp2[1];
		data[13][o] = fp2[5];
		data[14][o] = fp3[1];
		data[15][o] = fp3[5];
		o++;
		data[0][o]  = fp0[2];
		data[1][o]  = fp0[6];
		data[2][o]  = fp1[2];
		data[3][o]  = fp1[6];
		data[4][o]  = fp2[2];
		data[5][o]  = fp2[6];
		data[6][o]  = fp3[2];
		data[7][o]  = fp3[6];
		data[8][o]  = fp0[3];
		data[9][o]  = fp0[7];
		data[10][o] = fp1[3];
		data[11][o] = fp1[7];
		data[12][o] = fp2[3];
		data[13][o] = fp2[7];
		data[14][o] = fp3[3];
		data[15][o] = fp3[7];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 2*nblank;
}

static int mark4_decode_1bit_32track_fanout4(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/4;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			nblank++;
			i += 4;
		}
		else
		{
			fp0 = lut1bit[buf[i]];
			i++;
			fp1 = lut1bit[buf[i]];
			i++;
			fp2 = lut1bit[buf[i]];
			i++;
			fp3 = lut1bit[buf[i]];
			i++;
		}
		m++;

		data[0][o] = fp0[0];
		data[1][o] = fp1[0];
		data[2][o] = fp2[0];
		data[3][o] = fp3[0];
		data[4][o] = fp0[1];
		data[5][o] = fp1[1];
		data[6][o] = fp2[1];
		data[7][o] = fp3[1];
		o++;
		data[0][o] = fp0[2];
		data[1][o] = fp1[2];
		data[2][o] = fp2[2];
		data[3][o] = fp3[2];
		data[4][o] = fp0[3];
		data[5][o] = fp1[3];
		data[6][o] = fp2[3];
		data[7][o] = fp3[3];
		o++;
		data[0][o] = fp0[4];
		data[1][o] = fp1[4];
		data[2][o] = fp2[4];
		data[3][o] = fp3[4];
		data[4][o] = fp0[5];
		data[5][o] = fp1[5];
		data[6][o] = fp2[5];
		data[7][o] = fp3[5];
		o++;
		data[0][o] = fp0[6];
		data[1][o] = fp1[6];
		data[2][o] = fp2[6];
		data[3][o] = fp3[6];
		data[4][o] = fp0[7];
		data[5][o] = fp1[7];
		data[6][o] = fp2[7];
		data[7][o] = fp3[7];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 4*nblank;
}

static int mark4_decode_1bit_64track_fanout1(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3, *fp4, *fp5, *fp6, *fp7;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/8;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			fp4 = fp5 = fp6 = fp7 = zeros;
			nblank++;
			i += 8;
		}
		else
		{
			fp0 = lut1bit[buf[i]];
			i++;
			fp1 = lut1bit[buf[i]];
			i++;
			fp2 = lut1bit[buf[i]];
			i++;
			fp3 = lut1bit[buf[i]];
			i++;
			fp4 = lut1bit[buf[i]];
			i++;
			fp5 = lut1bit[buf[i]];
			i++;
			fp6 = lut1bit[buf[i]];
			i++;
			fp7 = lut1bit[buf[i]];
			i++;
		}
		m++;

		data[0][o]  = fp0[0];
		data[1][o]  = fp0[2];
		data[2][o]  = fp0[4];
		data[3][o]  = fp0[6];
		data[4][o]  = fp1[0];
		data[5][o]  = fp1[2];
		data[6][o]  = fp1[4];
		data[7][o]  = fp1[6];
		data[8][o]  = fp2[0];
		data[9][o]  = fp2[2];
		data[10][o] = fp2[4];
		data[11][o] = fp2[6];
		data[12][o] = fp3[0];
		data[13][o] = fp3[2];
		data[14][o] = fp3[4];
		data[15][o] = fp3[6];
		data[16][o] = fp0[1];
		data[17][o] = fp0[3];
		data[18][o] = fp0[5];
		data[19][o] = fp0[7];
		data[20][o] = fp1[1];
		data[21][o] = fp1[3];
		data[22][o] = fp1[5];
		data[23][o] = fp1[7];
		data[24][o] = fp2[1];
		data[25][o] = fp2[3];
		data[26][o] = fp2[5];
		data[27][o] = fp2[7];
		data[28][o] = fp3[1];
		data[29][o] = fp3[3];
		data[30][o] = fp3[5];
		data[31][o] = fp3[7];
		data[32][o] = fp4[0];
		data[33][o] = fp4[2];
		data[34][o] = fp4[4];
		data[35][o] = fp4[6];
		data[36][o] = fp5[0];
		data[37][o] = fp5[2];
		data[38][o] = fp5[4];
		data[39][o] = fp5[6];
		data[40][o] = fp6[0];
		data[41][o] = fp6[2];
		data[42][o] = fp6[4];
		data[43][o] = fp6[6];
		data[44][o] = fp7[0];
		data[45][o] = fp7[2];
		data[46][o] = fp7[4];
		data[47][o] = fp7[6];
		data[48][o] = fp4[1];
		data[49][o] = fp4[3];
		data[50][o] = fp4[5];
		data[51][o] = fp4[7];
		data[52][o] = fp5[1];
		data[53][o] = fp5[3];
		data[54][o] = fp5[5];
		data[55][o] = fp5[7];
		data[56][o] = fp6[1];
		data[57][o] = fp6[3];
		data[58][o] = fp6[5];
		data[59][o] = fp6[7];
		data[60][o] = fp7[1];
		data[61][o] = fp7[3];
		data[62][o] = fp7[5];
		data[63][o] = fp7[7];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - nblank;
}

static int mark4_decode_1bit_64track_fanout2(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3, *fp4, *fp5, *fp6, *fp7;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/8;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			fp4 = fp5 = fp6 = fp7 = zeros;
			nblank++;
			i += 8;
		}
		else
		{
			fp0 = lut1bit[buf[i]];
			i++;
			fp1 = lut1bit[buf[i]];
			i++;
			fp2 = lut1bit[buf[i]];
			i++;
			fp3 = lut1bit[buf[i]];
			i++;
			fp4 = lut1bit[buf[i]];
			i++;
			fp5 = lut1bit[buf[i]];
			i++;
			fp6 = lut1bit[buf[i]];
			i++;
			fp7 = lut1bit[buf[i]];
			i++;
		}
		m++;

		data[0][o]  = fp0[0];
		data[1][o]  = fp0[4];
		data[2][o]  = fp1[0];
		data[3][o]  = fp1[4];
		data[4][o]  = fp2[0];
		data[5][o]  = fp2[4];
		data[6][o]  = fp3[0];
		data[7][o]  = fp3[4];
		data[8][o]  = fp0[1];
		data[9][o]  = fp0[5];
		data[10][o] = fp1[1];
		data[11][o] = fp1[5];
		data[12][o] = fp2[1];
		data[13][o] = fp2[5];
		data[14][o] = fp3[1];
		data[15][o] = fp3[5];
		data[16][o] = fp4[0];
		data[17][o] = fp4[4];
		data[18][o] = fp5[0];
		data[19][o] = fp5[4];
		data[20][o] = fp6[0];
		data[21][o] = fp6[4];
		data[22][o] = fp7[0];
		data[23][o] = fp7[4];
		data[24][o] = fp4[1];
		data[25][o] = fp4[5];
		data[26][o] = fp5[1];
		data[27][o] = fp5[5];
		data[28][o] = fp6[1];
		data[29][o] = fp6[5];
		data[30][o] = fp7[1];
		data[31][o] = fp7[5];
		o++;
		data[0][o]  = fp0[2];
		data[1][o]  = fp0[6];
		data[2][o]  = fp1[2];
		data[3][o]  = fp1[6];
		data[4][o]  = fp2[2];
		data[5][o]  = fp2[6];
		data[6][o]  = fp3[2];
		data[7][o]  = fp3[6];
		data[8][o]  = fp0[3];
		data[9][o]  = fp0[7];
		data[10][o] = fp1[3];
		data[11][o] = fp1[7];
		data[12][o] = fp2[3];
		data[13][o] = fp2[7];
		data[14][o] = fp3[3];
		data[15][o] = fp3[7];
		data[16][o] = fp4[2];
		data[17][o] = fp4[6];
		data[18][o] = fp5[2];
		data[19][o] = fp5[6];
		data[20][o] = fp6[2];
		data[21][o] = fp6[6];
		data[22][o] = fp7[2];
		data[23][o] = fp7[6];
		data[24][o] = fp4[3];
		data[25][o] = fp4[7];
		data[26][o] = fp5[3];
		data[27][o] = fp5[7];
		data[28][o] = fp6[3];
		data[29][o] = fp6[7];
		data[30][o] = fp7[3];
		data[31][o] = fp7[7];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 2*nblank;
}

static int mark4_decode_1bit_64track_fanout4(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3, *fp4, *fp5, *fp6, *fp7;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/8;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			fp4 = fp5 = fp6 = fp7 = zeros;
			nblank++;
			i += 8;
		}
		else
		{
			fp0 = lut1bit[buf[i]];
			i++;
			fp1 = lut1bit[buf[i]];
			i++;
			fp2 = lut1bit[buf[i]];
			i++;
			fp3 = lut1bit[buf[i]];
			i++;
			fp4 = lut1bit[buf[i]];
			i++;
			fp5 = lut1bit[buf[i]];
			i++;
			fp6 = lut1bit[buf[i]];
			i++;
			fp7 = lut1bit[buf[i]];
			i++;
		}
		m++;
		
		data[0][o]  = fp0[0];
		data[1][o]  = fp1[0];
		data[2][o]  = fp2[0];
		data[3][o]  = fp3[0];
		data[4][o]  = fp0[1];
		data[5][o]  = fp1[1];
		data[6][o]  = fp2[1];
		data[7][o]  = fp3[1];
		data[8][o]  = fp4[0];
		data[9][o]  = fp5[0];
		data[10][o] = fp6[0];
		data[11][o] = fp7[0];
		data[12][o] = fp4[1];
		data[13][o] = fp5[1];
		data[14][o] = fp6[1];
		data[15][o] = fp7[1];
		o++;
		data[0][o]  = fp0[2];
		data[1][o]  = fp1[2];
		data[2][o]  = fp2[2];
		data[3][o]  = fp3[2];
		data[4][o]  = fp0[3];
		data[5][o]  = fp1[3];
		data[6][o]  = fp2[3];
		data[7][o]  = fp3[3];
		data[8][o]  = fp4[2];
		data[9][o]  = fp5[2];
		data[10][o] = fp6[2];
		data[11][o] = fp7[2];
		data[12][o] = fp4[3];
		data[13][o] = fp5[3];
		data[14][o] = fp6[3];
		data[15][o] = fp7[3];
		o++;
		data[0][o]  = fp0[4];
		data[1][o]  = fp1[4];
		data[2][o]  = fp2[4];
		data[3][o]  = fp3[4];
		data[4][o]  = fp0[5];
		data[5][o]  = fp1[5];
		data[6][o]  = fp2[5];
		data[7][o]  = fp3[5];
		data[8][o]  = fp4[4];
		data[9][o]  = fp5[4];
		data[10][o] = fp6[4];
		data[11][o] = fp7[4];
		data[12][o] = fp4[5];
		data[13][o] = fp5[5];
		data[14][o] = fp6[5];
		data[15][o] = fp7[5];
		o++;
		data[0][o]  = fp0[6];
		data[1][o]  = fp1[6];
		data[2][o]  = fp2[6];
		data[3][o]  = fp3[6];
		data[4][o]  = fp0[7];
		data[5][o]  = fp1[7];
		data[6][o]  = fp2[7];
		data[7][o]  = fp3[7];
		data[8][o]  = fp4[6];
		data[9][o]  = fp5[6];
		data[10][o] = fp6[6];
		data[11][o] = fp7[6];
		data[12][o] = fp4[7];
		data[13][o] = fp5[7];
		data[14][o] = fp6[7];
		data[15][o] = fp7[7];


		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 4*nblank;
}

/************************ 2-bit decoders *********************/

static int mark4_decode_2bit_8track_fanout1(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp;
	int o, i;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp = zeros;
			nblank++;
		}
		else
		{
			fp = lut2bit1[buf[i]];
		}
		i++;

		data[0][o] = fp[0];
		data[1][o] = fp[1];
		data[2][o] = fp[2];
		data[3][o] = fp[3];

		if(i >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
		}
	}

	ms->readposition = i;

	return nsamp - nblank;
}

static int mark4_decode_2bit_8track_fanout2(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp;
	int o, i;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp = zeros;
			nblank++;
		}
		else
		{
			fp = lut2bit2[buf[i]];
		}
		i++;

		data[0][o] = fp[0];
		data[1][o] = fp[2];
		o++;
		data[0][o] = fp[1];
		data[1][o] = fp[3];

		if(i >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 2*nblank;
}

static int mark4_decode_2bit_8track_fanout4(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp;
	int o, i;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp = zeros;
			nblank++;
		}
		else
		{
			fp = lut2bit3[buf[i]];
		}
		i++;

		data[0][o] = fp[0];
		o++;
		data[0][o] = fp[1];
		o++;
		data[0][o] = fp[2];
		o++;
		data[0][o] = fp[3];

		if(i >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 4*nblank;
}

static int mark4_decode_2bit_16track_fanout1(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/2;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = zeros;
			i += 2;
			nblank++;
		}
		else
		{
			fp0 = lut2bit1[buf[i]];
			i++;
			fp1 = lut2bit1[buf[i]];
			i++;
		}
		m++;

		data[0][o]  = fp0[0];
		data[1][o]  = fp0[1];
		data[2][o]  = fp0[2];
		data[3][o]  = fp0[3];
		data[4][o]  = fp1[0];
		data[5][o]  = fp1[1];
		data[6][o]  = fp1[2];
		data[7][o]  = fp1[3];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - nblank;
}

static int mark4_decode_2bit_16track_fanout2(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/2;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = zeros;
			i += 2;
			nblank++;
		}
		else
		{
			fp0 = lut2bit2[buf[i]];
			i++;
			fp1 = lut2bit2[buf[i]];
			i++;
		}
		m++;

		data[0][o] = fp0[0];
		data[1][o] = fp0[2];
		data[2][o] = fp1[0];
		data[3][o] = fp1[2];
		o++;
		data[0][o] = fp0[1];
		data[1][o] = fp0[3];
		data[2][o] = fp1[1];
		data[3][o] = fp1[3];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 2*nblank;
}

static int mark4_decode_2bit_16track_fanout4(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/2;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = zeros;
			i += 2;
			nblank++;
		}
		else
		{
			fp0 = lut2bit3[buf[i]];
			i++;
			fp1 = lut2bit3[buf[i]];
			i++;
		}
		m++;

		data[0][o] = fp0[0];
		data[1][o] = fp1[0];
		o++;
		data[0][o] = fp0[1];
		data[1][o] = fp1[1];
		o++;
		data[0][o] = fp0[2];
		data[1][o] = fp1[2];
		o++;
		data[0][o] = fp0[3];
		data[1][o] = fp1[3];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 4*nblank;
}

static int mark4_decode_2bit_32track_fanout1(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/4;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			i += 4;
			nblank++;
		}
		else
		{
			fp0 = lut2bit2[buf[i]];
			i++;
			fp1 = lut2bit2[buf[i]];
			i++;
			fp2 = lut2bit2[buf[i]];
			i++;
			fp3 = lut2bit2[buf[i]];
			i++;
		}
		m++;

		data[0][o]  = fp0[0];
		data[1][o]  = fp0[2];
		data[2][o]  = fp1[0];
		data[3][o]  = fp1[2];
		data[4][o]  = fp2[0];
		data[5][o]  = fp2[2];
		data[6][o]  = fp3[0];
		data[7][o]  = fp3[2];
		data[8][o]  = fp0[1];
		data[9][o]  = fp0[3];
		data[10][o] = fp1[1];
		data[11][o] = fp1[3];
		data[12][o] = fp2[1];
		data[13][o] = fp2[3];
		data[14][o] = fp3[1];
		data[15][o] = fp3[3];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - nblank;
}

static int mark4_decode_2bit_32track_fanout2(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/4;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			i += 4;
			nblank++;
		}
		else
		{
			fp0 = lut2bit3[buf[i]];
			i++;
			fp1 = lut2bit3[buf[i]];
			i++;
			fp2 = lut2bit3[buf[i]];
			i++;
			fp3 = lut2bit3[buf[i]];
			i++;
		}
		m++;

		data[0][o]  = fp0[0];
		data[1][o]  = fp1[0];
		data[2][o]  = fp2[0];
		data[3][o]  = fp3[0];
		data[4][o]  = fp0[1];
		data[5][o]  = fp1[1];
		data[6][o]  = fp2[1];
		data[7][o]  = fp3[1];
		o++;
		data[0][o]  = fp0[2];
		data[1][o]  = fp1[2];
		data[2][o]  = fp2[2];
		data[3][o]  = fp3[2];
		data[4][o]  = fp0[3];
		data[5][o]  = fp1[3];
		data[6][o]  = fp2[3];
		data[7][o]  = fp3[3];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 2*nblank;
}

static int mark4_decode_2bit_32track_fanout4(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint32_t *buf, bits;
	float *fp0, *fp1, *fp2, *fp3;
	int o, i;
	int zone, l2;
	int nblank = 0;
	uint8_t *bytes;

	buf = (uint32_t *)(ms->payload);
	i = ms->readposition >> 2;  /* note here that i counts 32-bit words */
	l2 = ms->log2blankzonesize - 2;

	bytes = (uint8_t *)(& bits);

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> l2;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			nblank++;
		}
		else
		{
			bits = reorder32(buf[i]);
			fp0 = lut2bit1[bytes[0]];
			fp1 = lut2bit1[bytes[1]];
			fp2 = lut2bit1[bytes[2]];
			fp3 = lut2bit1[bytes[3]];
		}
		i++;

		data[0][o] = fp0[0];
		data[1][o] = fp2[0];
		data[2][o] = fp1[0];
		data[3][o] = fp3[0];
		o++;
		data[0][o] = fp0[1];
		data[1][o] = fp2[1];
		data[2][o] = fp1[1];
		data[3][o] = fp3[1];
		o++;
		data[0][o] = fp0[2];
		data[1][o] = fp2[2];
		data[2][o] = fp1[2];
		data[3][o] = fp3[2];
		o++;
		data[0][o] = fp0[3];
		data[1][o] = fp2[3];
		data[2][o] = fp1[3];
		data[3][o] = fp3[3];

		if(i >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = (uint32_t *)(ms->payload);
			i = 0;
		}
	}

	ms->readposition = 4*i;

	return nsamp - 4*nblank;
}

static int mark4_decode_2bit_64track_fanout1(struct mark5_stream *ms, int nsamp,
	float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3, *fp4, *fp5, *fp6, *fp7;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/8;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			fp4 = fp5 = fp6 = fp7 = zeros;
			i += 8;
			nblank++;
		}
		else
		{
			fp0 = lut2bit2[buf[i]];
			i++;
			fp1 = lut2bit2[buf[i]];
			i++;
			fp2 = lut2bit2[buf[i]];
			i++;
			fp3 = lut2bit2[buf[i]];
			i++;
			fp4 = lut2bit2[buf[i]];
			i++;
			fp5 = lut2bit2[buf[i]];
			i++;
			fp6 = lut2bit2[buf[i]];
			i++;
			fp7 = lut2bit2[buf[i]];
			i++;
		}
		m++;

		data[0][o]  = fp0[0];
		data[1][o]  = fp0[2];
		data[2][o]  = fp1[0];
		data[3][o]  = fp1[2];
		data[4][o]  = fp2[0];
		data[5][o]  = fp2[2];
		data[6][o]  = fp3[0];
		data[7][o]  = fp3[2];
		data[8][o]  = fp0[1];
		data[9][o]  = fp0[3];
		data[10][o] = fp1[1];
		data[11][o] = fp1[3];
		data[12][o] = fp2[1];
		data[13][o] = fp2[3];
		data[14][o] = fp3[1];
		data[15][o] = fp3[3];
		data[16][o] = fp4[0];
		data[17][o] = fp4[2];
		data[18][o] = fp5[0];
		data[19][o] = fp5[2];
		data[20][o] = fp6[0];
		data[21][o] = fp6[2];
		data[22][o] = fp7[0];
		data[23][o] = fp7[2];
		data[24][o] = fp4[1];
		data[25][o] = fp4[3];
		data[26][o] = fp5[1];
		data[27][o] = fp5[3];
		data[28][o] = fp6[1];
		data[29][o] = fp6[3];
		data[30][o] = fp7[1];
		data[31][o] = fp7[3];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - nblank;
}

static int mark4_decode_2bit_64track_fanout2(struct mark5_stream *ms, 
	int nsamp, float **data)
{
	uint8_t *buf;
	float *fp0, *fp1, *fp2, *fp3, *fp4, *fp5, *fp6, *fp7;
	int o, i, m;
	int zone;
	int nblank = 0;

	buf = ms->payload;
	i = ms->readposition;
	m = i/8;

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> ms->log2blankzonesize;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			fp4 = fp5 = fp6 = fp7 = zeros;
			i += 8;
			nblank++;
		}
		else
		{
			fp0 = lut2bit3[buf[i]];
			i++;
			fp1 = lut2bit3[buf[i]];
			i++;
			fp2 = lut2bit3[buf[i]];
			i++;
			fp3 = lut2bit3[buf[i]];
			i++;
			fp4 = lut2bit3[buf[i]];
			i++;
			fp5 = lut2bit3[buf[i]];
			i++;
			fp6 = lut2bit3[buf[i]];
			i++;
			fp7 = lut2bit3[buf[i]];
			i++;
		}
		m++;
		
		data[0][o]  = fp0[0];
		data[1][o]  = fp1[0];
		data[2][o]  = fp2[0];
		data[3][o]  = fp3[0];
		data[4][o]  = fp0[1];
		data[5][o]  = fp1[1];
		data[6][o]  = fp2[1];
		data[7][o]  = fp3[1];
		data[8][o]  = fp4[0];
		data[9][o]  = fp5[0];
		data[10][o] = fp6[0];
		data[11][o] = fp7[0];
		data[12][o] = fp4[1];
		data[13][o] = fp5[1];
		data[14][o] = fp6[1];
		data[15][o] = fp7[1];
		o++;
		data[0][o]  = fp0[2];
		data[1][o]  = fp1[2];
		data[2][o]  = fp2[2];
		data[3][o]  = fp3[2];
		data[4][o]  = fp0[3];
		data[5][o]  = fp1[3];
		data[6][o]  = fp2[3];
		data[7][o]  = fp3[3];
		data[8][o]  = fp4[2];
		data[9][o]  = fp5[2];
		data[10][o] = fp6[2];
		data[11][o] = fp7[2];
		data[12][o] = fp4[3];
		data[13][o] = fp5[3];
		data[14][o] = fp6[3];
		data[15][o] = fp7[3];

		if(m >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = ms->payload;
			i = 0;
			m = 0;
		}
	}

	ms->readposition = i;

	return nsamp - 2*nblank;
}

static int mark4_decode_2bit_64track_fanout4(struct mark5_stream *ms, 
	int nsamp, float **data)
{
	uint64_t *buf, bits;
	float *fp0, *fp1, *fp2, *fp3, *fp4, *fp5, *fp6, *fp7;
	int o, i;
	int zone, l2;
	int nblank = 0;
	uint8_t *bytes;

	buf = (uint64_t *)(ms->payload);
	i = ms->readposition >> 3;  /* note that i here counts 64-bit words */
	l2 = ms->log2blankzonesize - 3;

	bytes = (uint8_t *)(& bits);

	for(o = 0; o < nsamp; o++)
	{
		zone = i >> l2;

		if(i <  ms->blankzonestartvalid[zone] ||
		   i >= ms->blankzoneendvalid[zone])
		{
			fp0 = fp1 = fp2 = fp3 = zeros;
			fp4 = fp5 = fp6 = fp7 = zeros;
			nblank++;
		}
		else
		{
			bits = reorder64(buf[i]);
			fp0 = lut2bit1[bytes[0]];
			fp1 = lut2bit1[bytes[1]];
			fp2 = lut2bit1[bytes[2]];
			fp3 = lut2bit1[bytes[3]];
			fp4 = lut2bit1[bytes[4]];
			fp5 = lut2bit1[bytes[5]];
			fp6 = lut2bit1[bytes[6]];
			fp7 = lut2bit1[bytes[7]];
		}
		i++;
		
		data[0][o] = fp0[0];
		data[1][o] = fp2[0];
		data[2][o] = fp1[0];
		data[3][o] = fp3[0];
		data[4][o] = fp4[0];
		data[5][o] = fp6[0];
		data[6][o] = fp5[0];
		data[7][o] = fp7[0];
		o++;
		data[0][o] = fp0[1];
		data[1][o] = fp2[1];
		data[2][o] = fp1[1];
		data[3][o] = fp3[1];
		data[4][o] = fp4[1];
		data[5][o] = fp6[1];
		data[6][o] = fp5[1];
		data[7][o] = fp7[1];
		o++;
		data[0][o] = fp0[2];
		data[1][o] = fp2[2];
		data[2][o] = fp1[2];
		data[3][o] = fp3[2];
		data[4][o] = fp4[2];
		data[5][o] = fp6[2];
		data[6][o] = fp5[2];
		data[7][o] = fp7[2];
		o++;
		data[0][o] = fp0[3];
		data[1][o] = fp2[3];
		data[2][o] = fp1[3];
		data[3][o] = fp3[3];
		data[4][o] = fp4[3];
		data[5][o] = fp6[3];
		data[6][o] = fp5[3];
		data[7][o] = fp7[3];

		if(i >= PAYLOADSIZE)
		{
			if(mark5_stream_next_frame(ms) < 0)
			{
				return -1;
			}
			buf = (uint64_t *)(ms->payload);
			i = 0;
		}
	}

	ms->readposition = 8*i;

	return nsamp - 4*nblank;
}

/******************************************************************/

static int mark5_format_mark4_make_formatname(struct mark5_stream *ms)
{
	struct mark5_format_mark4 *f;

	f = (struct mark5_format_mark4 *)(ms->formatdata);
	
	sprintf(ms->formatname, "MKIV1_%d-%d-%d-%d", f->fanout, ms->Mbps, 
		ms->nchan, ms->nbit);

	return 0;
}

static int mark5_format_mark4_init(struct mark5_stream *ms)
{
	struct mark5_format_mark4 *f;
	int bytes;
	int tol=5;
	int mjd1, sec1, ns1;
	int datarate;

	if(!ms)
	{
		fprintf(stderr, "mark5_format_mark4_init: ms = 0\n");
		return -1;
	}

	f = (struct mark5_format_mark4 *)(ms->formatdata);

	ms->samplegranularity = f->fanout;
	ms->framebytes = 20000*f->ntrack/8;
	ms->databytes = 20000*f->ntrack/8;
	ms->payloadoffset = 0;
	ms->framesamples = 20000*f->fanout;
	ms->format = MK5_FORMAT_MARK4;
	ms->blanker = blanker_mark4;
	if(ms->datawindow)
	{
		if(ms->datawindowsize < ms->framebytes)
		{
			return -1;
		}

		/* look through entire data window, up to 1Mibytes */
		bytes = ms->datawindowsize < (1<<20) ?
			ms->datawindowsize : (1<<20);
		ms->frameoffset = findfirstframe(ms->datawindow, bytes, 
			f->ntrack, tol);
		if(ms->frameoffset < 0)
		{
			return -1;
		}

		ms->frame = ms->datawindow + ms->frameoffset;
		ms->payload = ms->frame + ms->payloadoffset;

		ms->gettime(ms, &ms->mjd, &ms->sec, &ms->ns);
		ms->frame += ms->framebytes;
		ms->gettime(ms, &mjd1, &sec1, &ns1);
		ms->frame -= ms->framebytes;

		/* assume frame time less than 1 second, integer number of
		 * frames per second
		 */
		if(ns1 != ms->ns)
		{
			ms->framens = ns1 - ms->ns;
			if(ms->framens <= 0)
			{
				ms->framens += 1000000000;
			}
			ms->samprate = ms->framesamples*
				(1000000000/ms->framens);
			datarate = ms->samprate*ms->nbit*ms->nchan/1000000;
			if(datarate != ms->Mbps)
			{
				if(ms->Mbps > 0)
				{
					fprintf(stderr, "Warning -- data rate "
						"disagrees : %d != %d\n",
						datarate, ms->Mbps);
				}
				ms->Mbps = datarate;
			}
		}
		else
		{
			fprintf(stderr, "Warning -- rate calc. suspect\n");
		}
	}
	mark5_format_mark4_make_formatname(ms);

	return 0;
}

static int mark5_format_mark4_final(struct mark5_stream *ms)
{
	if(!ms)
	{
		return -1;
	}

	if(ms->formatdata)
	{
		free(ms->formatdata);
	}

	return 0;
}

static int one(const struct mark5_stream *ms)
{
	return 1;
}

struct mark5_format_generic *new_mark5_format_mark4(int Mbps, int nchan,
	int nbit, int fanout)
{
	static int first = 1;
	struct mark5_format_generic *f;
	struct mark5_format_mark4 *v;
	int decoderindex=0;
	int ntrack;

	ntrack = nchan*fanout*nbit;

	if(first)
	{
		initluts();
		first = 0;
	}

	if(nbit == 1)
	{
		decoderindex += 0;
	}
	else if(nbit == 2)
	{
		decoderindex += 12;
	}
	else
	{
		fprintf(stderr, "nbit must be 1 or 2\n");
		return 0;
	}

	if(ntrack == 8)
	{
		decoderindex += 0;
	}
	else if(ntrack == 16)
	{
		decoderindex += 3;
	}
	else if(ntrack == 32)
	{
		decoderindex += 6;
	}
	else if(ntrack == 64)
	{
		decoderindex += 9;
	}
	else
	{
		fprintf(stderr, "ntrack must be 8, 16, 32 or 64\n");
		return 0;
	}

	if(fanout == 1)
	{
		decoderindex += 0;
	}
	else if(fanout == 2)
	{
		decoderindex += 1;
	}
	else if(fanout == 4)
	{
		decoderindex += 2;
	}
	else
	{
		fprintf(stderr, "fanout must be 1, 2 or 4\n");
		return 0;
	}

	v = (struct mark5_format_mark4 *)malloc(
		sizeof(struct mark5_format_mark4));
	f = (struct mark5_format_generic *)malloc(
		sizeof(struct mark5_format_generic));

	v->ntrack = ntrack;
	v->fanout = fanout;
	v->decade = 0;	/* Assume years 2000 to 2010 initially */

	f->Mbps = Mbps;
	f->nchan = nchan;
	f->nbit = nbit;
	f->formatdata = v;
	f->gettime = mark5_format_mark4_frame_time;
	f->fixmjd = mark5_format_mark4_fixmjd;
	f->init_format = mark5_format_mark4_init;
	f->final_format = mark5_format_mark4_final;
	f->validate = one;
	switch(decoderindex)
	{
		case 0 : f->decode = mark4_decode_1bit_8track_fanout1; break;
		case 1 : f->decode = mark4_decode_1bit_8track_fanout2; break;
		case 2 : f->decode = mark4_decode_1bit_8track_fanout4; break;
		case 3 : f->decode = mark4_decode_1bit_16track_fanout1; break;
		case 4 : f->decode = mark4_decode_1bit_16track_fanout2; break;
		case 5 : f->decode = mark4_decode_1bit_16track_fanout4; break;
		case 6 : f->decode = mark4_decode_1bit_32track_fanout1; break;
		case 7 : f->decode = mark4_decode_1bit_32track_fanout2; break;
		case 8 : f->decode = mark4_decode_1bit_32track_fanout4; break;
		case 9 : f->decode = mark4_decode_1bit_64track_fanout1; break;
		case 10: f->decode = mark4_decode_1bit_64track_fanout2; break;
		case 11: f->decode = mark4_decode_1bit_64track_fanout4; break;
		case 12: f->decode = mark4_decode_2bit_8track_fanout1; break;
		case 13: f->decode = mark4_decode_2bit_8track_fanout2; break;
		case 14: f->decode = mark4_decode_2bit_8track_fanout4; break;
		case 15: f->decode = mark4_decode_2bit_16track_fanout1; break;
		case 16: f->decode = mark4_decode_2bit_16track_fanout2; break;
		case 17: f->decode = mark4_decode_2bit_16track_fanout4; break;
		case 18: f->decode = mark4_decode_2bit_32track_fanout1; break;
		case 19: f->decode = mark4_decode_2bit_32track_fanout2; break;
		case 20: f->decode = mark4_decode_2bit_32track_fanout4; break;
		case 21: f->decode = mark4_decode_2bit_64track_fanout1; break;
		case 22: f->decode = mark4_decode_2bit_64track_fanout2; break;
		case 23: f->decode = mark4_decode_2bit_64track_fanout4; break;
	}

	return f;
}

