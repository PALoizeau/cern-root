// @(#)root/proofplayer:$Id: TAdaptivePacketizer.cxx,v 1.18 2007/09/09 16:10:01 ganis Exp $
// Author: Jan Iwaszkiewicz   11/12/06

/*************************************************************************
 * Copyright (C) 1995-2002, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
*************************************************************************/

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// TAdaptivePacketizer                                                  //
//                                                                      //
// This packetizer is based on TPacketizer but uses different           //
// load-balancing algorithms and data structures.                       //
// Two main improvements in the load-balancing strategy:                //
// - First one was to change the order in which the files are assigned  //
//   to the computing nodes in such a way that network transfers are    //
//   evenly distributed in the query time. Transfer of the remote files //
//   was often becoming a bottleneck at the end of a query.             //
// - The other improvement is the use of time-based packet size. We     //
//   measure the processing rate of all the nodes and calculate the     //
//   packet size, so that it takes certain amount of time. In this way  //
//   packetizer prevents the situation where the query can't finish     //
//   because of one slow node.                                          //
//                                                                      //
// The data structures: TFileStat, TFileNode and TSlaveStat are         //
// enriched + changed and TFileNode::Compare method is changed.         //
//                                                                      //
//////////////////////////////////////////////////////////////////////////


#include "TAdaptivePacketizer.h"

#include "Riostream.h"
#include "TDSet.h"
#include "TError.h"
#include "TEnv.h"
#include "TEntryList.h"
#include "TEventList.h"
#include "TMap.h"
#include "TMessage.h"
#include "TMonitor.h"
#include "TNtupleD.h"
#include "TObject.h"
#include "TParameter.h"
#include "TPerfStats.h"
#include "TProofDebug.h"
#include "TProof.h"
#include "TProofPlayer.h"
#include "TProofServ.h"
#include "TSlave.h"
#include "TSocket.h"
#include "TTimer.h"
#include "TUrl.h"
#include "TClass.h"
#include "TRandom.h"
#include "TMath.h"
#include "TObjString.h"

//
// The following three utility classes manage the state of the
// work to be performed and the slaves involved in the process.
// A list of TFileNode(s) describes the hosts with files, each
// has a list of TFileStat(s) keeping the state for each TDSet
// element (file).
//
// The list of TSlaveStat(s) keep track of the work (being) done
// by each slave
//


//------------------------------------------------------------------------------

class TAdaptivePacketizer::TFileStat : public TObject {

private:
   Bool_t         fIsDone;       // is this element processed
   TFileNode     *fNode;         // my FileNode
   TDSetElement  *fElement;      // location of the file and its range
   Long64_t       fNextEntry;    // cursor in the range, -1 when done // needs changing

public:
   TFileStat(TFileNode *node, TDSetElement *elem);

   Bool_t         IsDone() const {return fIsDone;}
   void           SetDone() {fIsDone = kTRUE;}
   TFileNode     *GetNode() const {return fNode;}
   TDSetElement  *GetElement() const {return fElement;}
   Long64_t       GetNextEntry() const {return fNextEntry;}
   void           MoveNextEntry(Long64_t step) {fNextEntry += step;}
};


TAdaptivePacketizer::TFileStat::TFileStat(TFileNode *node, TDSetElement *elem)
   : fIsDone(kFALSE), fNode(node), fElement(elem), fNextEntry(elem->GetFirst())
{
}

//------------------------------------------------------------------------------

// a class describing a file node as a part of a session
class TAdaptivePacketizer::TFileNode : public TObject {

private:
   TString        fNodeName;        // FQDN of the node
   TList         *fFiles;           // TDSetElements (files) stored on this node
   TObject       *fUnAllocFileNext; // cursor in fFiles
   TList         *fActFiles;        // files with work remaining
   TObject       *fActFileNext;     // cursor in fActFiles
   Int_t          fMySlaveCnt;      // number of slaves running on this node
                                    // (which can process remote files)
   Int_t          fExtSlaveCnt;     // number of external slaves processing
                                    // files on this node
   Int_t          fRunSlaveCnt;     // total number of slaves processing files
                                    // on this node
   Long64_t       fProcessed;       // number of events processed on this node
   Long64_t       fEvents;          // number of entries in files on this node

public:
   TFileNode(const char *name);
   ~TFileNode() { delete fFiles; delete fActFiles; }

   void        IncMySlaveCnt() { fMySlaveCnt++; }
   Int_t       GetMySlaveCnt() const { return fMySlaveCnt; }
   void        IncExtSlaveCnt(const char *slave) { if (fNodeName != slave) fExtSlaveCnt++; }
   void        DecExtSlaveCnt(const char *slave) { if (fNodeName != slave) fExtSlaveCnt--; R__ASSERT(fExtSlaveCnt >= 0); }
   Int_t       GetSlaveCnt() const { return fMySlaveCnt + fExtSlaveCnt; }
   void        IncRunSlaveCnt() { fRunSlaveCnt++; }
   void        DecRunSlaveCnt() { fRunSlaveCnt--; R__ASSERT(fRunSlaveCnt >= 0); }
   Int_t       GetRunSlaveCnt() const { return fRunSlaveCnt; }
   Int_t       GetExtSlaveCnt() const { return fExtSlaveCnt; }
   Int_t       GetNumberOfActiveFiles() const { return fActFiles->GetSize(); }
   Bool_t      IsSortable() const { return kTRUE; }
   Int_t       GetNumberOfFiles() { return fFiles->GetSize(); }
   void        IncProcessed(Long64_t nEvents)
                  { fProcessed += nEvents; }
   Long64_t    GetProcessed() const { return fProcessed; }
   // this method is used by Compare() it adds 1, so it returns a number that
   // would be true if one more slave is added.
   Long64_t    GetEventsLeftPerSlave() const
      { return ((fEvents - fProcessed)/(fRunSlaveCnt + 1)); }
   void        IncEvents(Long64_t nEvents) { fEvents += nEvents; }
   const char *GetName() const { return fNodeName.Data(); }
   Long64_t    GetNEvents() const { return fEvents; }

   void Add(TDSetElement *elem)
   {
      TFileStat *f = new TFileStat(this,elem);
      fFiles->Add(f);
      if (fUnAllocFileNext == 0) fUnAllocFileNext = fFiles->First();
   }

   TFileStat *GetNextUnAlloc()
   {
      TObject *next = fUnAllocFileNext;

      if (next != 0) {
         // make file active
         fActFiles->Add(next);
         if (fActFileNext == 0) fActFileNext = fActFiles->First();

         // move cursor
         fUnAllocFileNext = fFiles->After(fUnAllocFileNext);
      }
      return (TFileStat *) next;
   }

   TFileStat *GetNextActive()
   {
      TObject *next = fActFileNext;

      if (fActFileNext != 0) {
         fActFileNext = fActFiles->After(fActFileNext);
         if (fActFileNext == 0) fActFileNext = fActFiles->First();
      }

      return (TFileStat *) next;
   }

   void RemoveActive(TFileStat *file)
   {
      if (fActFileNext == file) fActFileNext = fActFiles->After(file);
      fActFiles->Remove(file);
      if (fActFileNext == 0) fActFileNext = fActFiles->First();
   }

   Int_t Compare(const TObject *other) const
   {
      // Must return -1 if this is smaller than obj, 0 if objects are equal
      // and 1 if this is larger than obj.
      // smaller means more needing a new worker.
      // Two cases are considered depending on
      // relation between harddrive speed and network bandwidth.

      const TFileNode *obj = dynamic_cast<const TFileNode*>(other);
      R__ASSERT(obj != 0);
      // how many more events it has than obj

      if (fgNetworkFasterThanHD) {
         // The network bandwidth is greater than hard disk transfer.
         // Smaller means having fewer workers processing its data.
         // If equal, the numer of events left for processing decides.

         Int_t myVal = GetRunSlaveCnt();
         Int_t otherVal = obj->GetRunSlaveCnt();
         if (myVal < otherVal) {
            return -1;
         } else if (myVal > otherVal) {
            return 1;
         } else {
            // if this has more events to process than obj
            if ((fEvents - fProcessed) >
                (obj->GetNEvents() - obj->GetProcessed())) {
               return -1;
            } else {
               return 1;
            }
         }
      } else {
         // Network bandwidth is smaller than Hard Disk transfer
         // TODO: Do more tests when such environment will be available.

         // File nodes are sorted by number of remote workers.
         // In addition filenodes with the same number of remote workers
         // are sorted by number of their own ones.
         // If that is equal as well,
         // they are sorted by the number of events left per worker
         // The decision can be reversed if the diffEvents
         // is greater or smaller than half or 1/3 of avEventsLeft.

         Long64_t diffEvents =
            GetEventsLeftPerSlave() - obj->GetEventsLeftPerSlave();
         Int_t myExtSlaves = GetExtSlaveCnt();
         Int_t otherExtSlaves = obj->GetExtSlaveCnt();
         Long64_t avEventsLeft = (GetEventsLeftPerSlave()
                                  + obj->GetEventsLeftPerSlave())/2;
         // # my workers processing remote files
         Int_t mySlavesProcRemote = GetSlaveCnt() - GetRunSlaveCnt();
         Int_t otherSlavesProcRemote = obj->GetSlaveCnt()
                                       - obj->GetRunSlaveCnt();
         if ( mySlavesProcRemote < otherSlavesProcRemote ) {
            if (diffEvents < -(avEventsLeft / 2)
                && obj->GetExtSlaveCnt() < TAdaptivePacketizer::fgMaxSlaveCnt)
               return 1;
            else
               return -1;
         } else if ( mySlavesProcRemote > otherSlavesProcRemote ) {
            if (diffEvents > (avEventsLeft / 2)
                && GetExtSlaveCnt() < TAdaptivePacketizer::fgMaxSlaveCnt)
               return -1;
            else
               return 1;
         } else if (myExtSlaves < otherExtSlaves) {
            if (diffEvents < -(avEventsLeft / 3)
                && obj->GetExtSlaveCnt() < TAdaptivePacketizer::fgMaxSlaveCnt)
               return 1;
            else
               return -1;
         } else if (myExtSlaves > otherExtSlaves) {
            if (diffEvents > (avEventsLeft / 3)
                && GetExtSlaveCnt() < TAdaptivePacketizer::fgMaxSlaveCnt)
               return -1;
            else
               return 1;
         } else {
            Int_t myOwnSlaves = GetMySlaveCnt();
            Int_t otherOwnSlaves = obj->GetMySlaveCnt();
            if (myOwnSlaves < otherOwnSlaves) {
               // if the other node has much more to process
               if (diffEvents < -(avEventsLeft / 3)
                   && obj->GetExtSlaveCnt() < TAdaptivePacketizer::fgMaxSlaveCnt)
                  return 1;
               else
                  return -1;
            } else if (myOwnSlaves > otherOwnSlaves) {
               if (diffEvents > (avEventsLeft / 3)
                   && GetExtSlaveCnt() < TAdaptivePacketizer::fgMaxSlaveCnt)
                  return -1;
               else
                  return 1;
            } else {

            if (diffEvents > 0) {
                  return -1;
               } else if (diffEvents < 0) {
                  return 1;
               } else
                  return 0;
            }
         }
      }
   }

   void Print(Option_t *) const
   {
      cout << "OBJ: " << IsA()->GetName() << "\t" << fNodeName
           << "\tMySlaveCount " << fMySlaveCnt
           << "\tSlaveCount " << fExtSlaveCnt << endl;
   }

   void Reset()
   {
      fUnAllocFileNext = fFiles->First();
      fActFiles->Clear();
      fActFileNext = 0;
      fExtSlaveCnt = 0;
      fMySlaveCnt = 0;
      fRunSlaveCnt = 0;
   }
};


TAdaptivePacketizer::TFileNode::TFileNode(const char *name)
   : fNodeName(name), fFiles(new TList), fUnAllocFileNext(0),fActFiles(new TList),
     fActFileNext(0), fMySlaveCnt(0), fExtSlaveCnt(0), fProcessed(0), fEvents(0)
{
   // Constructor

   fFiles->SetOwner();
   fActFiles->SetOwner(kFALSE);
}

//------------------------------------------------------------------------------

class TAdaptivePacketizer::TSlaveStat : public TObject {

friend class TAdaptivePacketizer;

private:
   TSlave        *fSlave;        // corresponding TSlave record
   TFileNode     *fFileNode;     // corresponding node or 0
   TFileStat     *fCurFile;      // file currently being processed
   TDSetElement  *fCurElem;      // TDSetElement currently being processed
   Long64_t       fProcessed;    // number of entries processed
   Float_t        fProcTime;     // sum of processing time of packets
   Long64_t       fCurProcessed; // eventst processed in the current file
   Float_t        fCurProcTime;  // proc time spent on the current file

public:
   TSlaveStat(TSlave *slave);

   TFileNode  *GetFileNode() const { return fFileNode; }
   const char *GetName() const { return fSlave->GetName(); }
   Long64_t    GetEntriesProcessed() const { return fProcessed; }
   TFileStat  *GetCurFile() { return fCurFile; }
   void        SetFileNode(TFileNode *node) { fFileNode = node; }
   void        UpdateRates(Long64_t nEvents, Float_t time);
   Float_t     GetAvgRate() { return (fProcTime?fProcessed/fProcTime:0); }
   Float_t     GetCurRate() {
      return (fCurProcTime?fCurProcessed/fCurProcTime:0); }
   Int_t       GetLocalEventsLeft() {
      return fFileNode?(fFileNode->GetEventsLeftPerSlave()):0; }
};

//______________________________________________________________________________
TAdaptivePacketizer::TSlaveStat::TSlaveStat(TSlave *slave)
   : fSlave(slave), fFileNode(0), fCurFile(0), fCurElem(0), fProcessed(0),
     fProcTime(0), fCurProcessed(0), fCurProcTime(0)
{
   //constructor
}

//______________________________________________________________________________
void TAdaptivePacketizer::TSlaveStat::UpdateRates(Long64_t nEvents,
                                                  Float_t time)
{
   //Update packetizer rates
   if (fCurFile->IsDone()) {
      fCurProcTime = 0;
      fCurProcessed = 0;
   } else {
      fCurProcTime += time;
      fCurProcessed += nEvents;
   }
   fProcTime += time;
   fProcessed += nEvents;
   fCurFile->GetNode()->IncProcessed(nEvents);
}
//------------------------------------------------------------------------------

ClassImp(TAdaptivePacketizer)

Int_t TAdaptivePacketizer::fgMaxSlaveCnt = 2;
Int_t TAdaptivePacketizer::fgNetworkFasterThanHD = 1;

//______________________________________________________________________________
TAdaptivePacketizer::TAdaptivePacketizer(TDSet *dset, TList *slaves,
                          Long64_t first, Long64_t num, TList *input)
                    : TVirtualPacketizer(input)
{
   // Constructor

   PDB(kPacketizer,1) Info("TAdaptivePacketizer",
                           "enter (first %lld, num %lld)", first, num);

   // Init pointer members
   fSlaveStats = 0;
   fPackets = 0;
   fSlaveStats = 0;
   fUnAllocated = 0;
   fActive = 0;
   fFileNodes = 0;
   fCumProcTime = 0;
   fMaxPerfIdx = 1;

   Long_t maxSlaveCnt = 0;
   if (!(TProof::GetParameter(input, "PROOF_MaxSlavesPerNode", maxSlaveCnt))) {
      fgMaxSlaveCnt = (Int_t) maxSlaveCnt;
   } else {
      // Use number of CPUs as default
      SysInfo_t si;
      gSystem->GetSysInfo(&si);
      if (si.fCpus > 2)
         fgMaxSlaveCnt =  si.fCpus;
   }

   fgNetworkFasterThanHD = gEnv->GetValue("ProofServ.NetworkFasterThanHD", 1);
   if (fgNetworkFasterThanHD != 1)
      Info("TAdaptivePacketizer","fgNetworkFasterThanHD set to %d",
                                 fgNetworkFasterThanHD);

   Double_t baseLocalPreference = 1.2;
   TProof::GetParameter(input, "PROOF_BaseLocalPreference", baseLocalPreference);
   fBaseLocalPreference = (Float_t)baseLocalPreference;

   fPackets = new TList;
   fPackets->SetOwner();

   fFileNodes = new TList;
   fFileNodes->SetOwner();
   fUnAllocated = new TList;
   fUnAllocated->SetOwner(kFALSE);
   fActive = new TList;
   fActive->SetOwner(kFALSE);


   fValid = kTRUE;

   // Resolve end-point urls to optmize distribution
   dset->Lookup();

   // Split into per host entries
   dset->Reset();
   TDSetElement *e;
   while ((e = (TDSetElement*)dset->Next())) {
      if (e->GetValid()) continue;

      TUrl url = e->GetFileName();

      // Map non URL filenames to dummy host
      TString host;
      if ( !url.IsValid() ||
          (strncmp(url.GetProtocol(),"root", 4) &&
           strncmp(url.GetProtocol(),"rfio", 4)) ) {
         host = "no-host";
      } else {
         host = url.GetHost();
      }

      TFileNode *node = (TFileNode*) fFileNodes->FindObject( host );

      if (node == 0) {
         node = new TFileNode(host);
         fFileNodes->Add(node);
      }

      node->Add( e );
   }

   fSlaveStats = new TMap;
   fSlaveStats->SetOwner(kFALSE);

   TSlave *slave;
   TIter si(slaves);
   while ((slave = (TSlave*) si.Next())) {
      fSlaveStats->Add( slave, new TSlaveStat(slave) );
      fMaxPerfIdx = slave->GetPerfIdx() > fMaxPerfIdx ?
         slave->GetPerfIdx() : fMaxPerfIdx;
   }

   Reset();                // setup file & filenode structure
   ValidateFiles(dset, slaves);


   if (!fValid) return;

   // apply global range (first,num) to dset and rebuild structure
   // ommitting TDSet elements that are not needed

   Int_t files = 0;
   fTotalEntries = 0;
   fUnAllocated->Clear();  // avoid dangling pointers
   fActive->Clear();
   fFileNodes->Clear();    // then delete all objects
   PDB(kPacketizer,2)
      Info("TAdaptivePacketizer",
           "processing Range: First %lld, Num %lld", first, num);

   dset->Reset();
   Long64_t cur = 0;
   while (( e = (TDSetElement*)dset->Next())) {
      TUrl url = e->GetFileName();
      Long64_t eFirst = e->GetFirst();
      Long64_t eNum = e->GetNum();
      PDB(kPacketizer,2)
         Info("TAdaptivePacketizer",
              "processing element: First %lld, Num %lld (cur %lld)", eFirst, eNum, cur);

      if (!e->GetEntryList()) {
         // this element is before the start of the global range, skip it
         if (cur + eNum < first) {
            cur += eNum;
            PDB(kPacketizer,2)
               Info("TAdaptivePacketizer",
                    "processing element: skip element cur %lld", cur);
            continue;
         }

         // this element is after the end of the global range, skip it
         if (num != -1 && (first+num <= cur)) {
            cur += eNum;
            PDB(kPacketizer,2)
               Info("TAdaptivePacketizer",
                    "processing element: drop element cur %lld", cur);
            continue; // break ??
         }

         // If this element contains the end of the global range
         // adjust its number of entries
         if (num != -1 && (first+num < cur+eNum)) {
            e->SetNum( first + num - cur );
            PDB(kPacketizer,2)
               Info("TAdaptivePacketizer",
                    "processing element: Adjust end %lld", first + num - cur);
         }

         // If this element contains the start of the global range
         // adjust its start and number of entries
         if (cur < first) {
            e->SetFirst( eFirst + (first - cur) );
            e->SetNum( e->GetNum() - (first - cur) );
            PDB(kPacketizer,2)
               Info("TAdaptivePacketizer",
                    "processing element: Adjust start %lld and end %lld",
                    eFirst + (first - cur), first + num - cur);
         }

         cur += eNum;
      } else {
         Long64_t n = 0;
         TEntryList *enl = dynamic_cast<TEntryList *>(e->GetEntryList());
         if (enl) {
            n = enl->GetN();
         } else {
            TEventList *evl = dynamic_cast<TEventList *>(e->GetEntryList());
            n = evl ? evl->GetN() : n;
         }
         if (!n)
            continue;
      }
      PDB(kPacketizer,2)
         Info("TAdaptivePacketizer",
              "processing element: next cur %lld", cur);

      // Map non URL filenames to dummy host
      TString host;
      if ( !url.IsValid() ||
          (strncmp(url.GetProtocol(),"root", 4) &&
           strncmp(url.GetProtocol(),"rfio", 4)) ) {
         host = "no-host";
      } else {
         host = url.GetHost();
      }

      TFileNode *node = (TFileNode*) fFileNodes->FindObject( host );

      if ( node == 0 ) {
         node = new TFileNode( host );
         fFileNodes->Add( node );
      }

      ++files;
      fTotalEntries += e->GetNum();
      node->Add( e );
      node->IncEvents(e->GetNum());
      PDB(kPacketizer,2) e->Print("a");
   }
   // it overwrites previous value!!
   TEntryList *enl = dynamic_cast<TEntryList *>(dset->GetEntryList());
   if (enl) {
      fTotalEntries = enl->GetN();
   } else {
      TEventList *evl = dynamic_cast<TEventList *>(dset->GetEntryList());
      fTotalEntries = evl ? evl->GetN() : fTotalEntries;
   }
   PDB(kGlobal,1)
      Info("TAdaptivePacketizer", "processing %lld entries in %d files on %d hosts",
                                  fTotalEntries, files, fFileNodes->GetSize());
   Reset();

   // calculating how many files from TDSet are not cached on
   // any slave
   Int_t noRemoteFiles = 0;
   fNEventsOnRemLoc = 0;
   Int_t totalNumberOfFiles = 0;
   TIter next(fFileNodes);
   while (TFileNode *fn = (TFileNode*)next()) {
      totalNumberOfFiles += fn->GetNumberOfFiles();
      if (fn->GetSlaveCnt() == 0) {
         noRemoteFiles += fn->GetNumberOfFiles();
         fNEventsOnRemLoc += (fn->GetNEvents() - fn->GetProcessed());
      }
   }

   if (totalNumberOfFiles == 0) {
      Info("TAdaptivePacketizer", "no valid or non-empty file found: setting invalid");
      // No valid files: set invalid and return
      fValid = kFALSE;
      return;
   }

   fFractionOfRemoteFiles = noRemoteFiles / totalNumberOfFiles;
   Info("TAdaptivePacketizer",
        "fraction of remote files %f", fFractionOfRemoteFiles);

   if (!fValid)
      SafeDelete(fProgress);

   PDB(kPacketizer,1) Info("TAdaptivePacketizer", "return");
}

//______________________________________________________________________________
TAdaptivePacketizer::~TAdaptivePacketizer()
{
   // Destructor.

   if (fSlaveStats) {
      fSlaveStats->DeleteValues();
   }

   SafeDelete(fPackets);
   SafeDelete(fSlaveStats);
   SafeDelete(fUnAllocated);
   SafeDelete(fActive);
   SafeDelete(fFileNodes);
}

//______________________________________________________________________________
TAdaptivePacketizer::TFileStat *TAdaptivePacketizer::GetNextUnAlloc(TFileNode *node)
{
   // Get next unallocated file from 'node' or other nodes:
   // First try 'node'. If there is no more files, keep trying to
   // find an unallocated file on other nodes.

   TFileStat *file = 0;

   if (node != 0) {
      file = node->GetNextUnAlloc();
      if (file == 0) RemoveUnAllocNode(node);
   } else {
      while (file == 0 && ((node = NextNode()) != 0)) {
         file = node->GetNextUnAlloc();
         if (file == 0) RemoveUnAllocNode(node);
      }
   }

   if (file != 0) {
      // if needed make node active
      if (fActive->FindObject(node) == 0) {
         fActive->Add(node);
      }
   }

   return file;
}

//______________________________________________________________________________
TAdaptivePacketizer::TFileNode *TAdaptivePacketizer::NextNode()
{
   // Get next node which has unallocated files.
   // the order is determined by TFileNode::Compare

   fUnAllocated->Sort();
   PDB(kPacketizer,2) {
      fUnAllocated->Print();
   }

   TFileNode *fn = (TFileNode*) fUnAllocated->First();
   if (fn != 0 && fn->GetExtSlaveCnt() >= fgMaxSlaveCnt) {
      // unlike in TPacketizer we look at the number of ext slaves only.
      PDB(kPacketizer,1) Info("NextNode",
                              "Reached Slaves per Node Limit (%d)", fgMaxSlaveCnt);
      fn = 0;
   }

   return fn;
}

//______________________________________________________________________________
void TAdaptivePacketizer::RemoveUnAllocNode(TFileNode * node)
{
   // Remove unallocated node.

   fUnAllocated->Remove(node);
}

//______________________________________________________________________________
TAdaptivePacketizer::TFileStat *TAdaptivePacketizer::GetNextActive()
{
   // Get next active file.

   TFileNode *node;
   TFileStat *file = 0;

   while (file == 0 && ((node = NextActiveNode()) != 0)) {
         file = node->GetNextActive();
         if (file == 0) RemoveActiveNode(node);
   }

   return file;
}


//______________________________________________________________________________
TAdaptivePacketizer::TFileNode *TAdaptivePacketizer::NextActiveNode()
{
   // Get next active node.

   fActive->Sort();
   PDB(kPacketizer,2) {
      Info("NextActiveNode", "enter");
      fActive->Print();
   }

   TFileNode *fn = (TFileNode*) fActive->First();
   // look at only ext slaves
   if (fn != 0 && fn->GetExtSlaveCnt() >= fgMaxSlaveCnt) {
      PDB(kPacketizer,1)
         Info("NextActiveNode","reached Workers-per-Node limit (%d)", fgMaxSlaveCnt);
      fn = 0;
   }

   return fn;
}

//______________________________________________________________________________
void TAdaptivePacketizer::RemoveActive(TFileStat *file)
{
   // Remove file from the list of actives.

   TFileNode *node = file->GetNode();

   node->RemoveActive(file);
   if (node->GetNumberOfActiveFiles() == 0) RemoveActiveNode(node);
}

//______________________________________________________________________________
void TAdaptivePacketizer::RemoveActiveNode(TFileNode *node)
{
   // Remove node from the list of actives.

   fActive->Remove(node);
}

//______________________________________________________________________________
void TAdaptivePacketizer::Reset()
{
   // Reset the internal datastructure for packet distribution.

   fUnAllocated->Clear();
   fUnAllocated->AddAll(fFileNodes);

   fActive->Clear();

   TIter files(fFileNodes);
   TFileNode *fn;
   while ((fn = (TFileNode*) files.Next()) != 0) {
      fn->Reset();
   }

   TIter slaves(fSlaveStats);
   TObject *key;
   while ((key = slaves.Next()) != 0) {
      TSlaveStat *slstat = (TSlaveStat*) fSlaveStats->GetValue(key);
      TFileNode *fn = (TFileNode*) fFileNodes->FindObject(slstat->GetName());
      if (fn != 0 ) {
         slstat->SetFileNode(fn);
         fn->IncMySlaveCnt();
      }
      slstat->fCurFile = 0;
   }
}

//______________________________________________________________________________
void TAdaptivePacketizer::ValidateFiles(TDSet *dset, TList *slaves)
{
   // Check existence of file/dir/tree an get number of entries.
   // Assumes the files have been setup.

   TMap     slaves_by_sock;
   TMonitor mon;
   TList    workers;


   // Setup the communication infrastructure

   workers.AddAll(slaves);
   TIter    si(slaves);
   TSlave   *slave;
   while ((slave = (TSlave*)si.Next()) != 0) {
      PDB(kPacketizer,3) Info("ValidateFiles","socket added to monitor: %p (%s)",
          slave->GetSocket(), slave->GetName());
      mon.Add(slave->GetSocket());
      slaves_by_sock.Add(slave->GetSocket(),slave);
   }

   mon.DeActivateAll();

   ((TProof*)gProof)->DeActivateAsyncInput();

   // Some monitoring systems (TXSocketHandler) need to know this
   ((TProof*)gProof)->fCurrentMonitor = &mon;

   // Preparing for client notification
   TString msg("Validating files");
   UInt_t n = 0;
   UInt_t tot = dset->GetListOfElements()->GetSize();
   Bool_t st = kTRUE;

   while (kTRUE) {

      // send work
      while( TSlave *s = (TSlave*)workers.First() ) {

         workers.Remove(s);

         // find a file

         TSlaveStat *slstat = (TSlaveStat*)fSlaveStats->GetValue(s);
         TFileNode *node = 0;
         TFileStat *file = 0;

         // try its own node first
         if ( (node = slstat->GetFileNode()) != 0 ) {
            file = GetNextUnAlloc(node);
            if ( file == 0 ) {
               slstat->SetFileNode(0);
            }
         }

         // look for a file on any other node if necessary
         if (file == 0) {
            file = GetNextUnAlloc();
         }

         if ( file != 0 ) {
            // files are done right away
            RemoveActive(file);

            slstat->fCurFile = file;
            file->GetNode()->IncExtSlaveCnt(slstat->GetName());
            TMessage m(kPROOF_GETENTRIES);
            TDSetElement *elem = file->GetElement();
            m << dset->IsTree()
              << TString(elem->GetFileName())
              << TString(elem->GetDirectory())
              << TString(elem->GetObjName());

            s->GetSocket()->Send( m );
            mon.Activate(s->GetSocket());
            PDB(kPacketizer,2)
               Info("ValidateFiles",
                    "sent to slave-%s (%s) via %p GETENTRIES on %s %s %s %s",
                    s->GetOrdinal(), s->GetName(), s->GetSocket(),
                    dset->IsTree() ? "tree" : "objects",
                    elem->GetFileName(), elem->GetDirectory(), elem->GetObjName());
         }
      }

      if ( mon.GetActive() == 0 ) break; // nothing to wait for anymore

      PDB(kPacketizer,3) {
         Info("ValidateFiles", "waiting for %d slaves:", mon.GetActive());
         TList *act = mon.GetListOfActives();
         TIter next(act);
         while (TSocket *s = (TSocket*) next()) {
            TSlave *sl = (TSlave *) slaves_by_sock.GetValue(s);
            if (sl)
               Info("ValidateFiles", "   slave-%s (%s)", sl->GetOrdinal(), sl->GetName());
         }
         delete act;
      }

      TSocket *sock = mon.Select();
      mon.DeActivate(sock);

      PDB(kPacketizer,3) Info("ValidateFiles", "select returned: %p", sock);

      TSlave *slave = (TSlave *) slaves_by_sock.GetValue( sock );

      TMessage *reply;

      if ( sock->Recv(reply) <= 0 ) {
         // Help! lost a slave?
         ((TProof*)gProof)->MarkBad(slave);
         fValid = kFALSE;
         Error("ValidateFiles", "Recv failed! for slave-%s (%s)",
               slave->GetOrdinal(), slave->GetName());
         continue;
         }

      if ( reply->What() == kPROOF_FATAL ) {
         Error("ValidateFiles", "kPROOF_FATAL from slave-%s (%s)",
               slave->GetOrdinal(), slave->GetName());
         ((TProof*)gProof)->MarkBad(slave);
         fValid = kFALSE;
         continue;
      } else if ( reply->What() == kPROOF_LOGFILE ) {
         PDB(kPacketizer,3) Info("ValidateFiles", "got logfile");
         Int_t size;
         (*reply) >> size;
         ((TProof*)gProof)->RecvLogFile(sock, size);
         mon.Activate(sock);
         continue;
      } else if ( reply->What() == kPROOF_LOGDONE ) {
         PDB(kPacketizer,3) Info("ValidateFiles", "got logdone");
         mon.Activate(sock);
         continue;
      } else if ( reply->What() != kPROOF_GETENTRIES ) {
         // Help! unexpected message type
         Error("ValidateFiles", "unexpected message type (%d) from slave-%s (%s)",
               reply->What(), slave->GetOrdinal(), slave->GetName());
         ((TProof*)gProof)->MarkBad(slave);
         fValid = kFALSE;
         continue;
      }

      TSlaveStat *slavestat = (TSlaveStat*) fSlaveStats->GetValue( slave );
      TDSetElement *e = slavestat->fCurFile->GetElement();
      slavestat->fCurFile->GetNode()->DecExtSlaveCnt(slavestat->GetName());
      Long64_t entries;

      (*reply) >> entries;

      // Extract object name, if there
      if ((reply->BufferSize() > reply->Length())) {
         TString objname;
         (*reply) >> objname;
         e->SetTitle(objname);
      }

      e->SetTDSetOffset(entries);
      if ( entries > 0 ) {

         if (!e->GetEntryList()) {
            if ( e->GetFirst() > entries ) {
               Error("ValidateFiles", "first (%d) higher then number of entries (%d) in %d",
                     e->GetFirst(), entries, e->GetFileName() );

               // disable element
               slavestat->fCurFile->SetDone();
               fValid = kFALSE; // ???
            }

            if ( e->GetNum() == -1 ) {
               e->SetNum( entries - e->GetFirst() );
            } else if ( e->GetFirst() + e->GetNum() > entries ) {
               Error("ValidateFiles",
                     "Num (%d) + First (%d) larger then number of keys/entries (%d) in %s",
                     e->GetNum(), e->GetFirst(), entries, e->GetFileName() );
               e->SetNum( entries - e->GetFirst() );
            }
         }


         // Notify the client
         n++;
         gProof->SendDataSetStatus(msg, n, tot, st);

      } else {

         Error("ValidateFiles", "cannot get entries for %s (", e->GetFileName() );
         //
         // Need to fix this with a user option to allow incomplete file sets (rdm)
         //
         //fValid = kFALSE; // all element must be readable!
         if (gProofServ) {
            TMessage m(kPROOF_MESSAGE);
            m << TString(Form("Cannot get entries for file: %s - skipping", e->GetFileName()));
            gProofServ->GetSocket()->Send(m);
         }

         // disable element
         if (dset->Remove(e) == -1)
            Error("ValidateFiles", "removing of not-registered element %p failed", e);
      }

      workers.Add(slave); // Ready for the next job
   }

   // report std. output from slaves??

   ((TProof*)gProof)->ActivateAsyncInput();

   // This needs to be reset
   ((TProof*)gProof)->fCurrentMonitor = 0;

   // No reason to continue if invalid
   if (!fValid)
      return;

   // compute the offset for each file element
   Long64_t offset = 0;
   Long64_t newOffset = 0;
   TIter next(dset->GetListOfElements());
   TDSetElement *el;
   while ( (el = dynamic_cast<TDSetElement*> (next())) ) {
      newOffset = offset + el->GetTDSetOffset();
      el->SetTDSetOffset(offset);
      offset = newOffset;
   }
}

//______________________________________________________________________________
Long64_t TAdaptivePacketizer::GetEntriesProcessed(TSlave *slave) const
{
   // Get entries processed by the specified slave.

   if ( fSlaveStats == 0 ) return 0;

   TSlaveStat *slstat = (TSlaveStat*) fSlaveStats->GetValue( slave );

   if ( slstat == 0 ) return 0;

   return slstat->GetEntriesProcessed();
}

//______________________________________________________________________________
Int_t TAdaptivePacketizer::CalculatePacketSize(TObject *slStatPtr)
{
   // Calculates the packet size based on performance of this slave
   // and est. time left untill the end of the query.

   TSlaveStat* slstat = (TSlaveStat*)slStatPtr;
   Long64_t num;
   Int_t packetSizeAsFraction = 4;
   Float_t rate = slstat->GetCurRate();
   if (!rate)
      rate = slstat->GetAvgRate();
   if (rate) {
      Float_t avgProcRate = (fProcessed/(fCumProcTime / fSlaveStats->GetSize()));
      Float_t packetTime;
      packetTime = ((fTotalEntries - fProcessed)/avgProcRate)/packetSizeAsFraction;
      if (packetTime < 2)
         packetTime = 2;
      num = (Long64_t)(rate * packetTime);
   } else { //first packet for this slave in this query
      Int_t packetSize = (fTotalEntries - fProcessed)
                         / (8 * packetSizeAsFraction * fSlaveStats->GetSize());
      num = Long64_t(packetSize *
            ((Float_t)slstat->fSlave->GetPerfIdx() / fMaxPerfIdx));
   }
   if (num < 1) num = 1;

   return num;
}

//______________________________________________________________________________
TDSetElement *TAdaptivePacketizer::GetNextPacket(TSlave *sl, TMessage *r)
{
   // Get next packet;
   // A meaningfull difference to TPacketizer is the fact that this
   // packetizer, for each worker, tries to predict whether the worker
   // will finish processing it's local files before the end of the query.
   // If yes, it allocates, to those workers, files from non-slave filenodes
   // or from slaves that are overloaded. The check is done every time a new
   // file needs to be assigned.

   if ( !fValid ) {
      return 0;
   }

   // find slave

   TSlaveStat *slstat = (TSlaveStat*) fSlaveStats->GetValue( sl );

   R__ASSERT( slstat != 0 );

   // update stats & free old element

   if ( slstat->fCurElem != 0 ) {
      Double_t latency, proctime, proccpu;
      Long64_t bytesRead = -1;
      Long64_t totalEntries = -1;

      Long64_t numev = slstat->fCurElem->GetNum();

      fPackets->Add(slstat->fCurElem);
      (*r) >> latency >> proctime >> proccpu;
      // only read new info if available
      if (r->BufferSize() > r->Length()) (*r) >> bytesRead;
      if (r->BufferSize() > r->Length()) (*r) >> totalEntries;
      Long64_t totev = 0;
      if (r->BufferSize() > r->Length()) (*r) >> totev;

      // Record
      if (totev > 0)
         numev = totev - slstat->fProcessed;
      fProcessed += ((numev > 0) ? numev : 0);
      fBytesRead += ((bytesRead > 0) ? bytesRead : 0);

      // update processing rate
      slstat->UpdateRates(numev, proctime);
      fCumProcTime += proctime;

      PDB(kPacketizer,2)
         Info("GetNextPacket","slave-%s (%s): %lld %7.3lf %7.3lf %7.3lf %lld",
              sl->GetOrdinal(), sl->GetName(),
              numev, latency, proctime, proccpu, bytesRead);

      if (gPerfStats != 0) {
         gPerfStats->PacketEvent(sl->GetOrdinal(), sl->GetName(), slstat->fCurElem->GetFileName(),
                                 numev, latency, proctime, proccpu, bytesRead);
      }

      slstat->fCurElem = 0;
      if ( fProcessed == fTotalEntries ) {
         HandleTimer(0);   // Send last timer message
         delete fProgress; fProgress = 0;
      }

   }

   if ( fStop ) {
      HandleTimer(0);
      return 0;
   }

   TFileStat *file = slstat->fCurFile;
   // if current file is just finished
   if ( file != 0 && file->IsDone() ) {
      file->GetNode()->DecExtSlaveCnt(slstat->GetName());
      file->GetNode()->DecRunSlaveCnt();
      if (gPerfStats != 0) {
         gPerfStats->FileEvent(sl->GetOrdinal(), sl->GetName(), file->GetNode()->GetName(),
                               file->GetElement()->GetFileName(), kFALSE);
      }
      file = 0;
   }

   Long64_t avgEventsLeftPerSlave =
      (fTotalEntries - fProcessed) / fSlaveStats->GetSize();
   if (fTotalEntries == fProcessed)
      return 0;
   // get a file if needed
   if ( file == 0) {
      // needs a new file
      Bool_t openLocal;
      // aiming for localPreference == 1 when #local == #remote events left
      Float_t localPreference = fBaseLocalPreference - (fNEventsOnRemLoc /
                                (0.4 *(fTotalEntries - fProcessed)));
      if ( slstat->GetFileNode() != 0 ) {
         // local file node exists and has more events to process.
         fUnAllocated->Sort();
         TFileNode* firstNonLocalNode = (TFileNode*)fUnAllocated->First();
         Bool_t nonLocalNodePossible =
            firstNonLocalNode?(firstNonLocalNode->GetExtSlaveCnt() < fgMaxSlaveCnt):0;
         openLocal = !nonLocalNodePossible;
         Float_t slaveRate = slstat->GetAvgRate();
         if ( nonLocalNodePossible ) {
            // openLocal is set to kFALSE
            if ( slstat->GetFileNode()->GetRunSlaveCnt() >
                 slstat->GetFileNode()->GetMySlaveCnt() - 1 )
                // external slaves help slstat -> don't open nonlocal files
                // -1 because, at this point slstat is not running
                  openLocal = kTRUE;
            else if ( slaveRate == 0 ) { // first file for this slave
               // GetLocalEventsLeft() counts the potential slave
               // as running on its fileNode.
               if ( slstat->GetLocalEventsLeft() * localPreference > (avgEventsLeftPerSlave))
                  openLocal = kTRUE;
               else if ( (firstNonLocalNode->GetEventsLeftPerSlave())
                     < slstat->GetLocalEventsLeft() * localPreference )
                  openLocal = kTRUE;
               else if ( firstNonLocalNode->GetExtSlaveCnt() > 1 )
                  openLocal = kTRUE;
               else if ( firstNonLocalNode->GetRunSlaveCnt() == 0 )
                  openLocal = kTRUE;
            } else {
               // at this point slstat has a non zero avg rate > 0
               Float_t slaveTime = slstat->GetLocalEventsLeft()/slaveRate;
               // and thus fCumProcTime, fProcessed > 0
               Float_t avgTime = avgEventsLeftPerSlave/(fProcessed/fCumProcTime);
               if (slaveTime * localPreference > avgTime)
                  openLocal = kTRUE;
               else if ((firstNonLocalNode->GetEventsLeftPerSlave())
                        < slstat->GetLocalEventsLeft() * localPreference)
                  openLocal = kTRUE;
            }
         }
         if (openLocal) {
            // Try its own node
            file = slstat->GetFileNode()->GetNextUnAlloc();
            if (!file)
               file = slstat->GetFileNode()->GetNextActive();
            if ( file == 0 ) {
               //no more files on this slave.
               slstat->SetFileNode(0);
            }
         }
      }

      // try to find an unused filenode first
      if(file == 0) {
         file = GetNextUnAlloc();
      }

      // then look at the active filenodes
      if(file == 0) {
         file = GetNextActive();
      }

      if ( file == 0 ) return 0;

      slstat->fCurFile = file;
      // if remote and unallocated file
      if (file->GetNode()->GetMySlaveCnt() == 0 &&
         file->GetElement()->GetFirst() == file->GetNextEntry()) {
         fNEventsOnRemLoc -= file->GetElement()->GetNum();
         R__ASSERT(fNEventsOnRemLoc >= 0);
      }
      file->GetNode()->IncExtSlaveCnt(slstat->GetName());
      file->GetNode()->IncRunSlaveCnt();
      if (gPerfStats != 0) {
         gPerfStats->FileEvent(sl->GetOrdinal(), sl->GetName(),
                               file->GetNode()->GetName(),
                               file->GetElement()->GetFileName(), kTRUE);
      }
   }

   Long64_t num = CalculatePacketSize(slstat);

   // get a packet

   TDSetElement *base = file->GetElement();
   Long64_t first = file->GetNextEntry();
   Long64_t last = base->GetFirst() + base->GetNum();

   // if the remaining part is smaller than the packetsize, increase the packetsize

   if ( first + num >= last ) {
      num = last - first;
      file->SetDone(); // done

      // delete file from active list (unalloc list is single pass, no delete needed)
      RemoveActive(file);

   } else {
      file->MoveNextEntry(num);
   }

   slstat->fCurElem = CreateNewPacket(base, first, num);

   if (base->GetEntryList())
      slstat->fCurElem->SetEntryList(base->GetEntryList(), first, num);

   return slstat->fCurElem;
}
