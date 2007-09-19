// @(#)root/gl:$Id: TGLSelectBuffer.cxx,v 1.1 2007/06/11 19:56:34 brun Exp $
// Author:  Matevz Tadel, Feb 2007

/*************************************************************************
 * Copyright (C) 1995-2004, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "TGLSelectBuffer.h"
#include "TGLSelectRecord.h"
#include <TMath.h>

#include <algorithm>

/**************************************************************************/
/**************************************************************************/
// TGLSelectBuffer
/**************************************************************************/
/**************************************************************************/

Int_t TGLSelectBuffer::fgMaxBufSize = 1 << 20; // 1MByte

//______________________________________________________________________________
TGLSelectBuffer::TGLSelectBuffer() :
   fBufSize  (1024),
   fBuf      (new UInt_t [fBufSize]),
   fNRecords (-1)
{
   // Constructor.
}

//______________________________________________________________________________
TGLSelectBuffer::~TGLSelectBuffer()
{
   // Destructor.

   delete [] fBuf;
}

//______________________________________________________________________________
void TGLSelectBuffer::Grow()
{
   // Increase size of the select buffer.

   delete [] fBuf;
   fBufSize = TMath::Min(2*fBufSize, fgMaxBufSize);
   fBuf = new UInt_t[fBufSize];
}

//______________________________________________________________________________
void TGLSelectBuffer::ProcessResult(Int_t glResult)
{
   // Process result of GL-selection: sort the hits by their minimum
   // z-coordinate.

   // The '-1' case should be handled on the caller side.
   // Here we just assume no hits were recorded.

   if (glResult < 0)
      glResult = 0;

   fNRecords = glResult;
   fSortedRecords.resize(fNRecords);

   if (fNRecords > 0)
   {
      Int_t  i;
      UInt_t* buf = fBuf;
      for (i = 0; i < fNRecords; ++i)
      {
         fSortedRecords[i].first  = buf[1]; // minimum depth
         fSortedRecords[i].second = buf;    // record address
         buf += 3 + buf[0];
      }
      std::sort(fSortedRecords.begin(), fSortedRecords.end());
   }
}

//______________________________________________________________________________
void TGLSelectBuffer::SelectRecord(TGLSelectRecordBase& rec, Int_t i)
{
   // Return select record on (sorted) position i.

   rec.Set(fSortedRecords[i].second);
}
