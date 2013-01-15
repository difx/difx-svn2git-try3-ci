//============================================================================
// Name        : CX3cFile.h
// Author      : John Roberts, enhanced by Michael Johnson
// Version     :
// Copyright   : (C) Copyright 2010 XCube Research and Development
//               All rights reserved.
//
// Description : X3C File definitions and methods
//
// HISTORY     :
//       08/11/10 - Initial version....
//       10/20/10 - MGJ: added additional read/write/get/set methods
//       02/29/12 - JER: flip data file suffix to "lf1"
//============================================================================

#ifndef CX3CFILE_H_
#define CX3CFILE_H_

#include <time.h>
#include <fcntl.h>
#include <sys/time.h>
#include "xcTypes.h"

//Logger FILE TYPE SUFFIX, as in "myDataFile.x3c"
//Note there is not '.' file type separator defined here.
//History: For the first year and a half (2010, 2011) the Logger used the ".x3c"
//		File Suffix for DATA files. This was change to .lf1 Feb 28, 2012.
#define SUFFIX_LF1
#ifdef SUFFIX_LF1
 #define LOGDATA_FILE_SUFFIX_STR "lf1"
#else
 #define LOGDATA_FILE_SUFFIX_STR "x3c"
#endif

#define X3C_MAGIC     0x58334321
#define X3C_VERSION_MINOR 0x0001
#define X3C_VERSION_MAJOR 0x0000

/*
 * This is a timeval as stored in the X3C file.
 * It has to use the same types everywhere, independent of the actual
 * `struct timeval'. `struct timeval' has 32-bit tv_sec values on some
 * platforms and 64-bit tv_sec values on other platforms, and writing
 * out native `struct timeval' values would mean files could only be
 * read on systems with the same tv_sec size as the system on which
 * the file was written.
 */
struct x3c_timeval {
    UINT32 tv_sec;		//Seconds since 00:00:00 GMT, 1 January 1970
    union{
       UINT32 tv_usec;	// Additional microseconds since tv_sec
       UINT32 tv_nsec;	// Additional nanoseconds since tv_sec
    };
};

// GLOBAL FILE HEADER
typedef struct _X3C_HEADER_ {
		UINT32 magic;   		/* magic number */
		UINT16 version_major;  	/* major version number */
		UINT16 version_minor;  	/* minor version number */
		UINT16 task_type;      	/* defines data source  */
		UINT16 task_enum;
		struct x3c_timeval ts;  /* File Creation time	*/
		INT32  thiszone;       	/* GMT to local correction */
		UINT32 opt[2];          /* optional data		*/
		}X3C_HEADER;

// PACKET HEADER
typedef struct _X3C_PACKET_HEADER_ {
		struct x3c_timeval ts;
        UINT32 len;             /* packet data length */
} X3C_PACKET_HEADER;


class CX3cFile {
public:
	CX3cFile();
	virtual ~CX3cFile();

	// Init Data Buffer
	int    createDataBuffer(int len);
	int    getDataBufferSize() { return(mDataBufferSize); };

	// TBD
	X3C_HEADER        * getX3cHeaderPtr()    { return(&mX3cHeader); };
	X3C_PACKET_HEADER * getPacketHeaderPtr() { return(&mPacketHeader); };
	int    setX3cHeader(X3C_HEADER * pX3cHeader);
	int    setPacketHeader(X3C_PACKET_HEADER * pPacketHeader);

	int    getX3cHeaderLen()    { return(sizeof(X3C_HEADER)); };
	int    getPacketHeaderLen() { return(sizeof(X3C_PACKET_HEADER)); };
	int    getPacketLen()       { return((int)(sizeof(X3C_PACKET_HEADER) + mPacketDataLen)); };

	// Get/Set Packet Data
	char * getPacketDataPtr()   { return(mPacketData); };
	int    getPacketDataLen()   { return(mPacketDataLen); };
	int    setPacketData(char * buffer, UINT32 len);
	void   setPacketDataLen(UINT32 len) { mPacketDataLen = len; };


	// Get/Set X3C_Header
	UINT32 getX3cMagicNumber()  { return(mX3cHeader.magic); };
	UINT16 getX3cVersionMajor() { return(mX3cHeader.version_major); };
	UINT16 getX3cVersionMinor() { return(mX3cHeader.version_minor); };
	UINT16 getX3cTaskType()     { return(mX3cHeader.task_type); };
	UINT16 getX3cTaskEnum()     { return(mX3cHeader.task_enum); };
	INT32  getX3cTimeZone()     { return(mX3cHeader.thiszone); };
	INT32  getX3cOpt1()         { return(mX3cHeader.opt[0]); };
	INT32  getX3cOpt2()         { return(mX3cHeader.opt[1]); };
	INT32  getX3cTimeSec()      { return(mX3cHeader.ts.tv_sec); };
	INT32  getX3cTimeUSec()     { return(mX3cHeader.ts.tv_usec); };
	INT32  getX3cTimeNSec()     { return(mX3cHeader.ts.tv_nsec); };
	void   getX3cTime(timeval * tv) {  // sec and usec
				tv->tv_sec  = mX3cHeader.ts.tv_sec;
				tv->tv_usec = mX3cHeader.ts.tv_usec;
				return;
			};
	void   getX3cTime(timespec * ts) { // sec and nsec
				ts->tv_sec  = mX3cHeader.ts.tv_sec;
				ts->tv_nsec = mX3cHeader.ts.tv_nsec;
				return;
			};

	// Get/Set X3C_PACKET_HEADER
    UINT32 getPacketDataLength() { return(mPacketHeader.len); };
	INT32  getPacketTimeSec()    { return(mPacketHeader.ts.tv_sec); };
	INT32  getPacketTimeUSec()   { return(mPacketHeader.ts.tv_usec); };
	INT32  getPacketTimeNSec()   { return(mPacketHeader.ts.tv_nsec); };
	void   getPacketTime(timeval * tv) {  // sec and usec
				tv->tv_sec  = mPacketHeader.ts.tv_sec;
				tv->tv_usec = mPacketHeader.ts.tv_usec;
				return;
			};
	void   getPacketTime(timespec * ts) { // sec and nsec
				ts->tv_sec  = mPacketHeader.ts.tv_sec;
				ts->tv_nsec = mPacketHeader.ts.tv_nsec;
				return;
			};

	void getX3cTimeStamp(struct x3c_timeval * xts);

	// Utility methods
	void   printX3cHeader();
	void   printPacketHeader();
	void   printPacketData();

    // Legacy call, should be removed in the future
	int wrX3CHeader(INT32 *fd, UINT32 taskType, UINT32 taskEnum, INT32 thisZone);
	// this is not legacy but can be refactored - jtrier
    int wrX3CHeader(INT32 *fd, UINT32 taskType, UINT32 taskEnum, INT32 thisZone,
                    struct timeval tv, UINT32 opt1, UINT32 opt2);

private:
 	char *  mPacketData;      // allocated with the createDataBuffer
	UINT32  mPacketDataLen;
	UINT32  mDataBufferSize;

	X3C_HEADER         mX3cHeader;
	X3C_PACKET_HEADER  mPacketHeader;

};

#endif /* CX3CFILE_H_ */
