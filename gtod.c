/*
 * This software is available to you under the OpenFabrics.org BSD license
 * below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _GTOD_C_
#define _GTOD_C_

/*
 * int gettimeofday(struct timeval *ptv, void *ignored)
 */

#include <windows.h>
#include <winsock2.h>


static __inline
void FileTimeToTimeval(LPFILETIME pft, struct timeval * ptv)
{ /* Note that LONGLONG is a 64-bit value */
	LONGLONG ll;

	if (!pft || !ptv)
	{
		if (!ptv)
		{
			ptv->tv_sec = 0;
			ptv->tv_usec = 0;
		}
		return;
	}

	ll = ((LONGLONG) pft->dwHighDateTime << 32);
	ll += (LONGLONG) pft->dwLowDateTime;
	ll -= 116444736000000000;

	ptv->tv_sec = (long) (ll / 10000000);
	ptv->tv_usec = (long) (ll - ((LONGLONG)(ptv->tv_sec) * 10000000)) / 10;
}


int gettimeofday(struct timeval *ptv, void *ignored)
{
	static int QueryCounter = 2;
	static LARGE_INTEGER Frequency = {10000000,0}; /* prevent division by 0 */
	static LARGE_INTEGER Offset; /* counter offset for right time*/
	static LARGE_INTEGER LastCounter;
	FILETIME CurrentTime;
	UNREFERENCED_PARAMETER(ignored);     

	if(!ptv) return -1;

	if(QueryCounter)
	{
		LARGE_INTEGER Time;
		LARGE_INTEGER Counter;
	
		GetSystemTimeAsFileTime(&CurrentTime);
		QueryPerformanceCounter(&Counter);
	
		if(QueryCounter == 2)
		{
			if(QueryPerformanceFrequency(&Frequency))
			{
				QueryCounter = 1;
			}
			else
			{
				QueryCounter = 0;
				Frequency.QuadPart = 10000000; /* prevent division by 0 */
			}
	
			/* get time as a large integer */
			Counter.HighPart &= 0x7FL; /* Clear high bits to prevent overflow */
			Offset.LowPart = CurrentTime.dwLowDateTime;
			Offset.HighPart = (LONG) CurrentTime.dwHighDateTime;
			Offset.QuadPart -= Counter.QuadPart * 10000000 / Frequency.QuadPart;
		}
	
		/* Convert counter to a 100 nanoseconds resolution timer value. */
	
		Counter.HighPart &= 0x7FL; /* Clear high bits to prevent overflows */
		Counter.QuadPart *= 10000000; /* need time stamp in 100 ns units */
		Counter.QuadPart /= Frequency.QuadPart;/* counter of 0.1 microseconds */
	
		if(LastCounter.QuadPart > Counter.QuadPart)
		{ /* Counter value wrapped */
			Offset.QuadPart += (0x7F00000000*10000000) / Frequency.QuadPart;
		}
		LastCounter = Counter;
	
		/* Add the in previous call calculated offset */
		Counter.QuadPart += Offset.QuadPart;
	
		/* get time as a large integer */
		Time.LowPart = CurrentTime.dwLowDateTime;
		Time.HighPart = (LONG) CurrentTime.dwHighDateTime;
	
		/* keep time difference within an interval of +- 0.1 seconds
		relative to the time function by adjusting the counters offset */
	
		if( ((Time.QuadPart + 1000000) < Counter.QuadPart) ||
				((Time.QuadPart - 1000000) > Counter.QuadPart) )
		{ /* Adjust the offset */
			Offset.QuadPart += Time.QuadPart - Counter.QuadPart;
			Counter.QuadPart = Time.QuadPart;
		}
	
		/* use the adjusted performance counter time for the time stamp */
		CurrentTime.dwLowDateTime = Counter.LowPart;
		CurrentTime.dwHighDateTime = Counter.HighPart;
	}
	else
	{
		GetSystemTimeAsFileTime(&CurrentTime);
	}

	FileTimeToTimeval(&CurrentTime,ptv);

	return(0);
}

#endif /* _GTOD_C_ */
