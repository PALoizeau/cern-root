// @(#)root/proof:$Id: TDSet.cxx,v 1.18 2007/08/02 16:00:03 rdm Exp $
// Author: Fons Rademakers   11/01/02

/*************************************************************************
 * Copyright (C) 1995-2001, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// TDSet                                                                //
//                                                                      //
// This class implements a data set to be used for PROOF processing.    //
// The TDSet defines the class of which objects will be processed,      //
// the directory in the file where the objects of that type can be      //
// found and the list of files to be processed. The files can be        //
// specified as logical file names (LFN's) or as physical file names    //
// (PFN's). In case of LFN's the resolution to PFN's will be done       //
// according to the currently active GRID interface.                    //
// Examples:                                                            //
//   TDSet treeset("TTree", "AOD");                                     //
//   treeset.Add("lfn:/alien.cern.ch/alice/prod2002/file1");            //
//   ...                                                                //
//   treeset.AddFriend(friendset);                                      //
//                                                                      //
// or                                                                   //
//                                                                      //
//   TDSet objset("MyEvent", "*", "/events");                           //
//   objset.Add("root://cms.cern.ch/user/prod2002/hprod_1.root");       //
//   ...                                                                //
//   objset.Add(set2003);                                               //
//                                                                      //
// Validity of file names will only be checked at processing time       //
// (typically on the PROOF master server), not at creation time.        //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

#include "TDSet.h"

#include "Riostream.h"
#include "TClass.h"
#include "TClassTable.h"
#include "TCut.h"
#include "TError.h"
#include "TEntryList.h"
#include "TEventList.h"
#include "TFile.h"
#include "TFileInfo.h"
#include "TFileStager.h"
#include "TFriendElement.h"
#include "TKey.h"
#include "THashList.h"
#include "TMap.h"
#include "TROOT.h"
#include "TTimeStamp.h"
#include "TTree.h"
#include "TUrl.h"
#include "TRegexp.h"
#include "TVirtualPerfStats.h"
#include "TProof.h"
#include "TProofChain.h"
#include "TProofServ.h"
#include "TPluginManager.h"
#include "TChain.h"
#include "TChainElement.h"
#include "TSystem.h"
#include "THashList.h"

#include "TStreamerInfo.h"

ClassImp(TDSetElement)
ClassImp(TDSet)

//______________________________________________________________________________
TDSetElement::TDSetElement() : TNamed("",""),
                               fDirectory(),
                               fFirst(0),
                               fNum(0),
                               fMsd(),
                               fTDSetOffset(0),
                               fEntryList(0),
                               fValid(kFALSE),
                               fEntries(0),
                               fFriends(0)
{
   // Default constructor
   ResetBit(kWriteV3);
}

//______________________________________________________________________________
TDSetElement::TDSetElement(const char *file, const char *objname, const char *dir,
                           Long64_t first, Long64_t num,
                           const char *msd) : TNamed(file, objname)
{
   // Create a TDSet element.

   if (first < 0) {
      Warning("TDSetElement", "first must be >= 0, %d is not allowed - setting to 0", first);
      fFirst = 0;
   } else {
      fFirst = first;
   }
   if (num < -1) {
      Warning("TDSetElement", "num must be >= -1, %d is not allowed - setting to -1", num);
      fNum   = -1;
   } else {
      fNum   = num;
   }
   fMsd         = msd;
   fTDSetOffset = 0;
   fEntryList   = 0;
   fFriends     = 0;
   fValid       = kFALSE;
   fEntries     = -1;

   if (dir)
      fDirectory = dir;

   ResetBit(kWriteV3);
}

//______________________________________________________________________________
TDSetElement::TDSetElement(const TDSetElement& elem)
             : TNamed(elem.GetFileName(), elem.GetObjName())
{
   // copy constructor
   fDirectory = elem.GetDirectory();
   fFirst = elem.fFirst;
   fNum = elem.fNum;
   fMsd = elem.fMsd;
   fTDSetOffset = elem.fTDSetOffset;
   fEntryList = 0;
   fValid = elem.fValid;
   fEntries = elem.fEntries;
   fFriends = 0;
   ResetBit(kWriteV3);
}

//______________________________________________________________________________
TDSetElement::~TDSetElement()
{
   // Clean up the element.
   DeleteFriends();
}

//______________________________________________________________________________
const char *TDSetElement::GetDirectory() const
{
   // Return directory where to look for object.

   return fDirectory;
}

//______________________________________________________________________________
void TDSetElement::Print(Option_t *opt) const
{
   // Print a TDSetElement. When option="a" print full data.

   if (opt && opt[0] == 'a') {
      cout << IsA()->GetName()
           << " file=\"" << GetName()
           << "\" dir=\"" << fDirectory
           << "\" obj=\"" << GetTitle()
           << "\" first=" << fFirst
           << " num=" << fNum
           << " msd=\"" << fMsd
           << "\"" << endl;
   } else
      cout << "\tLFN: " << GetName() << endl;
}

//______________________________________________________________________________
void TDSetElement::Validate(Bool_t isTree)
{
   // Validate by opening the file.

   Long64_t entries = GetEntries(isTree);
   if (entries < 0) return; // Error should be reported by GetEntries()
   if (fFirst < entries) {
      if (fNum == -1) {
         fNum = entries - fFirst;
         fValid = kTRUE;
      } else {
         if (fNum <= entries - fFirst) {
            fValid = kTRUE;
         } else {
            Error("Validate", "TDSetElement has only %d entries starting"
                  " with entry %d, while %d were requested",
                  entries - fFirst, fFirst, fNum);
         }
      }
   } else {
      Error("Validate", "TDSetElement has only %d entries with"
            " first entry requested as %d", entries, fFirst);
   }
}

//______________________________________________________________________________
void TDSetElement::Validate(TDSetElement *elem)
{
   // Validate by checking against another element.

   // NOTE: Since this function validates against another TDSetElement,
   //       if the other TDSetElement (elem) did not use -1 to request all
   //       entries, this TDSetElement may get less than all entries if it
   //       requests all (with -1). For the application it was developed for
   //       (TProofSuperMaster::ValidateDSet) it works, since the design was
   //       to send the elements to their mass storage domain and let them
   //       look at the file and send the info back to the supermaster. The
   //       ability to set fValid was also required to be only exist in
   //       TDSetElement through certain function and not be set externally.
   //       TDSetElement may need to be extended for more general applications.

   if (!elem || !elem->GetValid()) {
      Error("Validate", "TDSetElement to validate against is not valid");
      return;
   }

   if (!strcmp(GetFileName(), elem->GetFileName()) &&
       !strcmp(GetDirectory(), elem->GetDirectory()) &&
       !strcmp(GetObjName(), elem->GetObjName())) {
      Long64_t entries = elem->fFirst + elem->fNum;
      if (fFirst < entries) {
         if (fNum == -1) {
            fNum = entries - fFirst;
            fValid = kTRUE;
         } else {
            if (fNum <= entries - fFirst) {
               fValid = kTRUE;
            } else {
               Error("Validate", "TDSetElement requests %d entries starting"
                     " with entry %d, while TDSetElement to validate against"
                     " has only %d entries", fNum, fFirst, entries);
            }
         }
      } else {
         Error("Validate", "TDSetElement to validate against has only %d"
               " entries, but this TDSetElement requested %d as its first"
               " entry", entries, fFirst);
      }
   } else {
      Error("Validate", "TDSetElements do not refer to same objects");
   }
}

//______________________________________________________________________________
Int_t TDSetElement::Compare(const TObject *obj) const
{
   //Compare elements by filename (and the fFirst).

   if (this == obj) return 0;

   const TDSetElement *elem = dynamic_cast<const TDSetElement*>(obj);
   if (!elem) {
      if (obj)
         return (strncmp(GetName(),obj->GetName(),strlen(GetName()))) ? 1 : 0;
      return -1;
   }

   Int_t order = strncmp(GetName(),elem->GetFileName(),strlen(GetName()));
   if (order == 0) {
      if (GetFirst() < elem->GetFirst())
         return -1;
      else if (GetFirst() > elem->GetFirst())
         return 1;
      return 0;
   }
   return order;
}

//______________________________________________________________________________
void TDSetElement::AddFriend(TDSetElement *friendElement, const char *alias)
{
   // Add friend TDSetElement to this set. The friend element will be copied to this object.

   if (!friendElement) {
      Error("AddFriend", "The friend TDSetElement is null!");
      return;
   }
   if (!fFriends) {
      fFriends = new TList();
      fFriends->SetOwner();
   }
   fFriends->Add(new TPair(new TDSetElement(*friendElement), new TObjString(alias)));
}

//______________________________________________________________________________
void TDSetElement::DeleteFriends()
{
   // Deletes the list of friends and all the friends on the list.
   if (!fFriends)
      return;

   TIter nxf(fFriends);
   TPair *p = 0;
   while ((p = (TPair *) nxf())) {
      delete p->Key();
      delete p->Value();
   }
   delete fFriends;
   fFriends = 0;
}

//______________________________________________________________________________
TDSetElement *TDSet::Next(Long64_t /*totalEntries*/)
{
   // Returns next TDSetElement.

   if (!fIterator) {
      fIterator = new TIter(fElements);
   }

   fCurrent = (TDSetElement *) fIterator->Next();
   return fCurrent;
}

//______________________________________________________________________________
Long64_t TDSetElement::GetEntries(Bool_t isTree)
{
   // Returns number of entries in tree or objects in file. Returns -1 in
   // case of error.

   if (fEntries > -1)
      return fEntries;

   Double_t start = 0;
   if (gPerfStats != 0) start = TTimeStamp();

   TFile *file = TFile::Open(GetName());

   if (gPerfStats != 0) {
      gPerfStats->FileOpenEvent(file, GetName(), double(TTimeStamp())-start);
   }

   if (file == 0) {
      ::SysError("TDSet::GetEntries", "cannot open file %s", GetName());
      return -1;
   }

   // Record end-point Url and mark as looked-up
   fName = ((TUrl *)file->GetEndpointUrl())->GetUrl();
   SetBit(kHasBeenLookedUp);

   TDirectory *dirsave = gDirectory;
   if (!file->cd(fDirectory)) {
      Error("GetEntries", "cannot cd to %s", fDirectory.Data());
      delete file;
      return -1;
   }

   TDirectory *dir = gDirectory;
   dirsave->cd();

   if (isTree) {

      TString on(GetTitle());
      TString sreg(GetTitle());
      // If a wild card we will use the first object of the type
      // requested compatible with the reg expression we got
      if (sreg.Length() <= 0 || sreg == "" || sreg.Contains("*")) {
         if (sreg.Contains("*"))
            sreg.ReplaceAll("*", ".*");
         else
            sreg = ".*";
         TRegexp re(sreg);
         if (dir->GetListOfKeys()) {
            TIter nxk(dir->GetListOfKeys());
            TKey *k = 0;
            Bool_t notfound = kTRUE;
            while ((k = (TKey *) nxk())) {
               if (!strcmp(k->GetClassName(), "TTree")) {
                  TString kn(k->GetName());
                  if (kn.Index(re) != kNPOS) {
                     if (notfound) {
                        on = kn;
                        notfound = kFALSE;
                     } else if (kn != on) {
                       Warning("GetEntries",
                               "additional tree found in the file: %s", kn.Data());
                     }
                  }
               }
            }
         }
      }

      TKey *key = dir->GetKey(on);
      if (key == 0) {
         Error("GetEntries", "cannot find tree \"%s\" in %s",
               GetTitle(), GetName());
         delete file;
         return -1;
      }
      TTree *tree = (TTree *) key->ReadObj();
      if (tree == 0) {
         // Error always reported?
         delete file;
         return -1;
      }
      fEntries = tree->GetEntries();
      delete tree;

   } else {
      TList *keys = dir->GetListOfKeys();
      fEntries = keys->GetSize();
   }

   delete file;
   return fEntries;
}

//______________________________________________________________________________
void TDSetElement::Lookup(Bool_t force)
{
   // Resolve end-point URL for this element
   static Int_t xNetPluginOK = -1;
   static TString xNotRedir;
   static TFileStager *xStager = 0;

   // Check if required
   if (!force && HasBeenLookedUp())
      return;

   // Open the file as raw to avoid the (slow) initialization
   TUrl url(GetName());
   // Save current options and anchor
   TString anch = url.GetAnchor();
   TString opts = url.GetOptions();
   // The full path
   TString name(url.GetUrl());

   // Depending on the type of backend, it might not make any sense to lookup
   Bool_t doit = kFALSE;
   TFile::EFileType type = TFile::GetType(name, "");
   if (type == TFile::kNet) {
      TPluginHandler *h = 0;
      // Network files via XROOTD
      if (xNetPluginOK == -1) {
         // Check the plugin the first time
         xNetPluginOK = 0;
         if ((h = gROOT->GetPluginManager()->FindHandler("TFile", name)) &&
            !strcmp(h->GetClass(),"TXNetFile") && h->LoadPlugin() == 0)
            xNetPluginOK = 1;
      }
      doit = (xNetPluginOK == 1) ? kTRUE : kFALSE;

      // The server may not be redirector: we might know this from the past
      // experience, if any
      if (xNotRedir.Length() > 0) {
         TUrl u(GetName());
         TString hp(Form("|%s:%d|", u.GetHostFQDN(), u.GetPort()));
         if (xNotRedir.Contains(hp))
            doit = kFALSE;
      }
   }

   // Do it by opening and closing the file. Ideally we could just
   // AccessPathName the path, but the TXNetSystem implementation is very
   // slow. To be fixed.
   if (doit) {
      if (!xStager || !xStager->Matches(name)) {
         SafeDelete(xStager);
         if (!(xStager = TFileStager::Open(name)))
            Error("Lookup", "TFileStager instance cannot be instantiated");
      }
      if (xStager && xStager->Locate(name.Data(), name) == 0) {
         // Get the effective end-point Url
         url.SetUrl(name);
         // Restore original options and anchor, if any
         url.SetOptions(opts);
         url.SetAnchor(anch);
         // Save it into the element
         fName = url.GetUrl();
      } else {
         // Failure
         Error("Lookup", "couldn't lookup %s\n", name.Data());
      }
   }

   // Mark has looked-up
   SetBit(kHasBeenLookedUp);
}

//______________________________________________________________________________
void TDSetElement::SetEntryList(TObject *aList, Long64_t first, Long64_t num)
{
   // Set entry (or event) list for this element

   if (!aList)
      return;

   // Link the proper object
   TEventList *evl = 0;
   TEntryList *enl = dynamic_cast<TEntryList *>(aList);
   if (!enl)
      evl = dynamic_cast<TEventList *>(aList);
   if (!enl && !evl) {
      Error("SetEntryList", "type of input object must be either TEntryList "
                            "or TEventList (found: '%s' - do nothing", aList->ClassName());
      return;
   }

   // Action depends on the type
   if (enl) {
      enl->SetEntriesToProcess(num);
   } else {
      for (; num > 0; num--, first++)
         evl->Enter(evl->GetEntry((Int_t)first));
   }
   fEntryList = aList;

   // Done
   return;
}

//______________________________________________________________________________
TDSet::TDSet()
{
   // Default ctor.

   fElements = new THashList;
   fElements->SetOwner();
   fIsTree    = kFALSE;
   fIterator  = 0;
   fCurrent   = 0;
   fEntryList = 0;
   fProofChain = 0;
   ResetBit(kWriteV3);

   // Add to the global list
   gROOT->GetListOfDataSets()->Add(this);
}

//______________________________________________________________________________
TDSet::TDSet(const char *name,
             const char *objname, const char *dir, const char *type)
{
   // Create a named TDSet object. The "type" defines the class of which objects
   // will be processed (default 'TTree'). The optional "objname" argument
   // specifies the name of the objects of the specified class.
   // If the "objname" is not given the behaviour depends on the 'type':
   // for 'TTree' the first TTree is analyzed; for other types, all objects of
   // the class found in the specified directory are processed.
   // The "dir" argument specifies in which directory the objects are
   // to be found, the top level directory ("/") is the default.
   // Directories can be specified using wildcards, e.g. "*" or "/*"
   // means to look in all top level directories, "/dir/*" in all
   // directories under "/dir", and "/*/*" to look in all directories
   // two levels deep.
   // For backward compatibility the type can also be passed via 'name',
   // in which case 'type' is ignored.

   fElements = new THashList;
   fElements->SetOwner();
   fIterator = 0;
   fCurrent  = 0;
   fEntryList = 0;
   fProofChain = 0;
   ResetBit(kWriteV3);

   fType = "TTree";
   TClass *c = 0;
   // Check name
   if (name && strlen(name) > 0) {
      // In the old constructor signature it was the 'type'
      if (!type) {
         if ((c = TClass::GetClass(name)))
            fType = name;
         else
            // Default type is 'TTree'
            fName = name;
      } else {
         // Set name
         fName = name;
         // Check type
         if (strlen(type) > 0)
            if ((c = TClass::GetClass(type)))
               fType = type;
      }
   } else if (type && strlen(type) > 0) {
      // Check the type
      if ((c = TClass::GetClass(type)))
         fType = type;
   }
   // The correct class type
   c = TClass::GetClass(fType);

   fIsTree = (c->InheritsFrom("TTree")) ? kTRUE : kFALSE;

   if (objname)
      fObjName = objname;

   if (dir)
      fDir = dir;

   // Default name is the object name
   if (fName.Length() <= 0)
      fName = fObjName;
   // We set the default title to the 'type'
   fTitle = fType;

   // Add to the global list
   gROOT->GetListOfDataSets()->Add(this);
}

//______________________________________________________________________________
TDSet::TDSet(const TChain &chain, Bool_t withfriends)
{
   // Create a named TDSet object from existing TChain 'chain'.
   // If 'withfriends' is kTRUE add also friends.
   // This constructor substituted the static methods TChain::MakeTDSet
   // removing any residual dependence of 'tree' on 'proof'.

   fElements = new THashList;
   fElements->SetOwner();
   fIterator = 0;
   fCurrent  = 0;
   fEntryList = 0;
   fProofChain = 0;
   ResetBit(kWriteV3);

   fType = "TTree";
   fIsTree = kTRUE;
   fObjName = chain.GetName();

   // First fill elements without friends()
   TIter next(chain.GetListOfFiles());
   TChainElement *elem;
   while ((elem = (TChainElement *)next())) {
      TString file(elem->GetTitle());
      TString tree(elem->GetName());
      Int_t isl = tree.Index("/");
      TString dir = "/";
      if (isl >= 0) {
         // Copy the tree name specification
         TString behindSlash = tree(isl + 1, tree.Length() - isl - 1);
         // and remove it from basename
         tree.Remove(isl);
         dir = tree;
         tree = behindSlash;
      }
      if (Add(file, tree, dir)) {
         if (elem->HasBeenLookedUp()) {
            // Save lookup information, if any
            TDSetElement *dse = (TDSetElement *) fElements->Last();
            if (dse)
               dse->SetLookedUp();
         }
      }
   }
   SetDirectory(0);

   // Add friends now, if requested
   if (withfriends) {
      TList processed;
      TList chainsQueue;
      chainsQueue.Add((TObject *)&chain);
      processed.Add((TObject *)&chain);
      while (chainsQueue.GetSize() > 0) {
         TChain *c = (TChain *) chainsQueue.First();
         chainsQueue.Remove(c);
         TIter friendsIter(c->GetListOfFriends());
         while(TFriendElement *fe = dynamic_cast<TFriendElement*> (friendsIter()) ) {
            if (TChain *fc = dynamic_cast<TChain*>(fe->GetTree())) {
               if (!processed.FindObject(fc)) {    // if not yet processed
                  processed.AddFirst(fc);
                  AddFriend(new TDSet((const TChain &)(*fc), kFALSE), fe->GetName());
                  chainsQueue.Add(fc);                        // for further processing
               }
            } else {
               Reset();
               Error("TDSet", "Only TChains supported. Found illegal tree %s",
                              fe->GetTree()->GetName());
               return;
            }
         }
      }
   }
}

//______________________________________________________________________________
TDSet::~TDSet()
{
   // Cleanup.

   SafeDelete(fElements);
   SafeDelete(fIterator);
   SafeDelete(fProofChain);

   gROOT->GetListOfDataSets()->Remove(this);
}

//______________________________________________________________________________
Long64_t TDSet::Process(const char *selector, Option_t *option, Long64_t nentries,
                        Long64_t first, TObject *enl)
{
   // Process TDSet on currently active PROOF session.
   // The last argument 'enl' specifies an entry- or event-list to be used as
   // event selection.
   // The return value is -1 in case of error and TSelector::GetStatus() in
   // in case of success.

   if (!IsValid() || !fElements->GetSize()) {
      Error("Process", "not a correctly initialized TDSet");
      return -1;
   }

   // Set entry list
   SetEntryList(enl);

   if (gProof)
      return gProof->Process(this, selector, option, nentries, first);

   Error("Process", "no active PROOF session");
   return -1;
}

//______________________________________________________________________________
void TDSet::AddInput(TObject *obj)
{
   // Add objects that might be needed during the processing of
   // the selector (see Process()).

   if (gProof) {
      gProof->AddInput(obj);
   } else {
      Error("AddInput","No PROOF session active");
   }
}

//______________________________________________________________________________
void TDSet::ClearInput()
{
   // Clear input object list.

   if (gProof)
      gProof->ClearInput();
}

//______________________________________________________________________________
TObject *TDSet::GetOutput(const char *name)
{
   // Get specified object that has been produced during the processing
   // (see Process()).

   if (gProof)
      return gProof->GetOutput(name);
   return 0;
}

//______________________________________________________________________________
TList *TDSet::GetOutputList()
{
   // Get list with all object created during processing (see Process()).

   if (gProof)
      return gProof->GetOutputList();
   return 0;
}

//______________________________________________________________________________
void TDSet::Print(const Option_t *opt) const
{
   // Print TDSet basic or full data. When option="a" print full data.

   cout <<"OBJ: " << IsA()->GetName() << "\ttype " << GetName() << "\t"
        << fObjName << "\tin " << GetTitle()
        << "\telements " << GetListOfElements()->GetSize() << endl;

   if (opt && opt[0] == 'a') {
      TIter next(GetListOfElements());
      TObject *obj;
      while ((obj = next())) {
         obj->Print(opt);
      }
   }
}

//______________________________________________________________________________
void TDSet::SetObjName(const char *objname)
{
   // Set/change object name.

   if (objname)
      fObjName = objname;
}

//______________________________________________________________________________
void TDSet::SetDirectory(const char *dir)
{
   // Set/change directory.

   if (dir)
      fDir = dir;
}

//______________________________________________________________________________
Bool_t TDSet::Add(const char *file, const char *objname, const char *dir,
                  Long64_t first, Long64_t num, const char *msd)
{
   // Add file to list of files to be analyzed. Optionally with the
   // objname and dir arguments the default, TDSet wide, objname and
   // dir can be overridden.

   if (!file || !*file) {
      Error("Add", "file name must be specified");
      return kFALSE;
   }

   // check, if it already exists in the TDSet
   TDSetElement *el = (TDSetElement *) fElements->FindObject(file);
   if (el) {
      Warning("Add", "duplicate, %40s is already in dataset, ignored", file);
      return kFALSE;
   }
   if (!objname)
      objname = GetObjName();
   if (!dir)
      dir = GetDirectory();

   fElements->Add(new TDSetElement(file, objname, dir, first, num, msd));

   return kTRUE;
}

//______________________________________________________________________________
Bool_t TDSet::Add(TDSet *dset)
{
   // Add specified data set to the this set.

   if (!dset)
      return kFALSE;

   if (fType != dset->GetType()) {
      Error("Add", "cannot add a set with a different type");
      return kFALSE;
   }

   TDSetElement *el;
   TIter next(dset->fElements);
   TObject *last = (dset == this) ? fElements->Last() : 0;
   while ((el = (TDSetElement*) next())) {
      Add(el->GetFileName(), el->GetObjName(), el->GetDirectory(),
          el->GetFirst(), el->GetNum(), el->GetMsd());
      if (el == last) break;
   }

   return kTRUE;
}

//______________________________________________________________________________
Bool_t TDSet::Add(TCollection *filelist)
{
   // Add files passed as list of TFileInfo, TUrl or TObjString objects .
   // If TFileInfo, the first entry and the number of entries are also filled.

   if (!filelist)
      return kFALSE;

   TObject *o = 0;
   TIter next(filelist);
   while ((o = next())) {
      TString cn(o->ClassName());
      if (cn == "TFileInfo") {
         TFileInfo *fi = (TFileInfo *)o;
         TFileInfoMeta *m = fi->GetMetaData();
         if (!m)
            Add(fi->GetFirstUrl()->GetUrl());
         else
            Add(fi->GetFirstUrl()->GetUrl(), m->GetObject(), m->GetDirectory(),
                m->GetFirst(), m->GetEntries());
      } else if (cn == "TUrl") {
         Add(((TUrl *)o)->GetUrl());
      } else if (cn == "TObjString") {
         Add(((TObjString *)o)->GetName());
      } else {
         Warning("Add","found object fo unexpected type %s - ignoring", cn.Data());
      }
   }

   return kTRUE;
}

//______________________________________________________________________________
Int_t TDSet::ExportFileList(const char *fpath, Option_t *opt)
{
   // Export TDSetElements files as list of TFileInfo objects in file
   // 'fpath'. If the file exists already the action fails, unless
   // 'opt == "F"'.
   // Return 0 on success, -1 otherwise

   if (!fElements)
      return -1;
   if (fElements->GetSize() <= 0)
      return 0;

   Bool_t force = (opt[0] == 'F' || opt[0] == 'f');

   if (gSystem->AccessPathName(fpath, kFileExists) == kFALSE) {
      if (force) {
         // Try removing the file
         if (gSystem->Unlink(fpath)) {
            Info("ExportFileList","error removing dataset file: %s", fpath);
            return -1;
         }
      }
   }

   // Create the file list
   TList *fileinfo = new TList;
   fileinfo->SetOwner();

   TDSetElement *dse = 0;
   TIter next(fElements);
   while ((dse = (TDSetElement *) next())) {
      TFileInfoMeta *m = new TFileInfoMeta(dse->GetTitle(), dse->GetDirectory(), GetType(),
                                           dse->GetNum(), dse->GetFirst());
      TFileInfo *fi = new TFileInfo(dse->GetFileName());
      fi->AddMetaData(m);
      fileinfo->Add(fi);
   }

   // Write to file
   TFile *f = TFile::Open(fpath, "RECREATE");
   if (f) {
      f->cd();
      fileinfo->Write("fileList", TObject::kSingleKey);
      f->Close();
   } else {
      Info("ExportFileList","error creating dataset file: %s", fpath);
      SafeDelete(fileinfo);
      return -1;
   }

   // Cleanup
   SafeDelete(f);
   SafeDelete(fileinfo);

   // We are done
   return 0;
}

//______________________________________________________________________________
void TDSet::AddFriend(TDSet *friendset, const char* alias)
{
   // Add friend dataset to this set. Only possible if the TDSet type is
   // a TTree or derived class. The friendset will be owned by this class
   // and deleted in its destructor.

   if (!friendset) {
      Error("AddFriend", "The friend TDSet is null!");
      return;
   }

   if (!fIsTree) {
      Error("AddFriend", "a friend set can only be added to a TTree TDSet");
      return;
   }
   TList* thisList = GetListOfElements();
   TList* friendsList = friendset->GetListOfElements();
   if (thisList->GetSize() != friendsList->GetSize() && friendsList->GetSize() != 1) {
      Error("AddFriend", "The friend Set has %d elements while the main one has %d",
            thisList->GetSize(), friendsList->GetSize());
      return;
   }
   TIter next(thisList);
   TIter next2(friendsList);
   TDSetElement* friendElem = 0;
   TString aliasString(alias);
   if (friendsList->GetSize() == 1)
      friendElem = dynamic_cast<TDSetElement*> (friendsList->First());
   while(TDSetElement* e = dynamic_cast<TDSetElement*> (next())) {
      if (friendElem) // just one elem in the friend TDSet
         e->AddFriend(friendElem, aliasString);
      else
         e->AddFriend(dynamic_cast<TDSetElement*> (next2()), aliasString);
   }
}

//______________________________________________________________________________
void TDSet::Reset()
{
   // Reset or initialize access to the elements.

   if (!fIterator) {
      fIterator = new TIter(fElements);
   } else {
      fIterator->Reset();
   }
}

//______________________________________________________________________________
Long64_t TDSet::GetEntries(Bool_t isTree, const char *filename, const char *path,
                           TString &objname)
{
   // Returns number of entries in tree or objects in file. Returns -1 in
   // case of error.

   Double_t start = 0;
   if (gPerfStats != 0) start = TTimeStamp();

   TFile *file = TFile::Open(filename);

   if (gPerfStats != 0) {
      gPerfStats->FileOpenEvent(file, filename, double(TTimeStamp())-start);
   }

   if (file == 0) {
      ::SysError("TDSet::GetEntries", "cannot open file %s", filename);
      return -1;
   }

   TDirectory *dirsave = gDirectory;
   if (!file->cd(path)) {
      ::Error("TDSet::GetEntries", "cannot cd to %s", path);
      delete file;
      return -1;
   }

   TDirectory *dir = gDirectory;
   dirsave->cd();

   Long64_t entries;
   Bool_t fillname = kFALSE;
   if (isTree) {

      TString on(objname);
      TString sreg(objname);
      // If a wild card we will use the first object of the type
      // requested compatible with the reg expression we got
      if (sreg.Length() <= 0 || sreg == "" || sreg.Contains("*")) {
         fillname = kTRUE;
         if (sreg.Contains("*"))
            sreg.ReplaceAll("*", ".*");
         else
            sreg = ".*";
         TRegexp re(sreg);
         if (dir->GetListOfKeys()) {
            TIter nxk(dir->GetListOfKeys());
            TKey *k = 0;
            Bool_t notfound = kTRUE;
            while ((k = (TKey *) nxk())) {
               if (!strcmp(k->GetClassName(), "TTree")) {
                  TString kn(k->GetName());
                  if (kn.Index(re) != kNPOS) {
                     if (notfound) {
                        on = kn;
                        notfound = kFALSE;
                     } else if (kn != on) {
                       ::Warning("TDSet::GetEntries",
                                 "additional tree found in the file: %s", kn.Data());
                     }
                  }
               }
            }
         }
      }

      TKey *key = dir->GetKey(on);
      if (key == 0) {
         ::Error("TDSet::GetEntries", "cannot find tree \"%s\" in %s",
                 objname.Data(), filename);
         delete file;
         return -1;
      }
      TTree *tree = (TTree *) key->ReadObj();
      if (tree == 0) {
         // Error always reported?
         delete file;
         return -1;
      }
      entries = tree->GetEntries();
      delete tree;

      // Return full name in case of wildcards
      objname = (fillname) ? on : objname;

   } else {
      TList *keys = dir->GetListOfKeys();
      entries = keys->GetSize();
   }

   delete file;
   return entries;
}

//______________________________________________________________________________
Long64_t TDSet::Draw(const char *varexp, const TCut &selection, Option_t *option,
                     Long64_t nentries, Long64_t firstentry)
{
   // Draw expression varexp for specified entries.
   // Returns -1 in case of error or number of selected events in case of success.
   // This function accepts a TCut objects as argument.
   // Use the operator+ to concatenate cuts.
   // Example:
   //   dset.Draw("x",cut1+cut2+cut3);

   return Draw(varexp, selection.GetTitle(), option, nentries, firstentry);
}

//______________________________________________________________________________
Long64_t TDSet::Draw(const char *varexp, const char *selection, Option_t *option,
                     Long64_t nentries, Long64_t firstentry)
{
   // Draw expression varexp for specified entries.
   // Returns -1 in case of error or number of selected events in case of success.
   // For more see TTree::Draw().

   if (!IsValid() || !fElements->GetSize()) {
      Error("Draw", "not a correctly initialized TDSet");
      return -1;
   }

   if (gProof)
      return gProof->DrawSelect(this, varexp, selection, option, nentries,
                                firstentry);

   Error("Draw", "no active PROOF session");
   return -1;
}

//_______________________________________________________________________
void TDSet::StartViewer()
{
   // Start the TTreeViewer on this TTree.

   if (gROOT->IsBatch()) {
      Warning("StartViewer", "viewer cannot run in batch mode");
      return;
   }

   if (!gProof) {
      Error("StartViewer", "no PROOF found");
      return;
   }
   if (!IsTree()) {
      Error("StartViewer", "TDSet contents should be of type TTree (or subtype)");
      return;
   }
   fProofChain = new TProofChain(this, kTRUE);

   TPluginHandler *h;
   if ((h = gROOT->GetPluginManager()->FindHandler("TVirtualTreeViewer"))) {
      if (h->LoadPlugin() == -1)
         return;
      h->ExecPlugin(1,fProofChain);
   }
}

//_______________________________________________________________________
TTree* TDSet::GetTreeHeader(TProof* proof)
{
   // Returns a tree header containing the branches' structure of the dataset.

   return proof->GetTreeHeader(this);
}

//______________________________________________________________________________
Bool_t TDSet::ElementsValid() const
{
   // Check if all elemnts are valid.

   TIter nextElem(GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextElem())) {
      if (!elem->GetValid()) return kFALSE;
   }
   return kTRUE;
}

//______________________________________________________________________________
Int_t TDSet::Remove(TDSetElement *elem)
{
   // Remove TDSetElement 'elem' from the list.
   // Return 0 on success, -1 if the element is not in the list

   if (!elem || !(GetListOfElements()->Remove(elem)))
      return -1;

   SafeDelete(elem);
   return 0;
}

//______________________________________________________________________________
void TDSet::Validate()
{
   // Validate the TDSet by opening files.

   TIter nextElem(GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextElem())) {
      if (!elem->GetValid())
         elem->Validate(IsTree());
   }
}

//______________________________________________________________________________
void TDSet::Lookup()
{
   // Resolve the end-point URL for the current elements of this data set
   // If an entry- or event- list has been given, assign the relevant portions
   // to each element; this also allows to look-up only for the elements which
   // have something to be processed, so it is better to do it before Lookup()

   // If an entry- or event- list has been given, assign the relevant portions
   // to each element; this allows to look-up only for the elements which have
   // something to be processed, so it is better to do it before the real look-up
   // operations.
   SplitEntryList();

   TString msg("Looking up for exact location of files");
   UInt_t n = 0;
   UInt_t ng = 0;
   UInt_t tot = GetListOfElements()->GetSize();
   UInt_t n2 = (tot > 50) ? (UInt_t) tot / 50 : 1;
   Bool_t st = kTRUE;
   TIter nextElem(GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextElem())) {
      if (elem->GetNum() != 0) { // -1 means "all entries"
         ng++;
         if (!elem->GetValid())
            elem->Lookup();
      }
      n++;
      // Notify the client
      if (gProof && (n > 0 && !(n % n2)))
         gProof->SendDataSetStatus(msg, n, tot, st);
   }
   // Notify the client if not all the files have entries to be processed
   // (which may happen if an entry-list is used)
   if (ng < tot && gProofServ) {
      msg = Form("Files with entries to be processed: %d (out of %d)\n", ng, tot);
      gProofServ->SendAsynMessage(msg);
   }
}

//______________________________________________________________________________
void TDSet::SetLookedUp()
{
   // Flag all the elements as looked-up, so to avoid opening the files
   // if the functionality is not supported

   TIter nextElem(GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextElem()))
      elem->SetLookedUp();
}

//______________________________________________________________________________
void TDSet::Validate(TDSet* dset)
{
   // Validate the TDSet against another TDSet.
   // Only validates elements in common from input TDSet.

   THashList bestElements;
   bestElements.SetOwner();
   TList namedHolder;
   namedHolder.SetOwner();
   TIter nextOtherElem(dset->GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextOtherElem())) {
      if (!elem->GetValid()) continue;
      TString dir_file_obj = elem->GetDirectory();
      dir_file_obj += "_";
      dir_file_obj += elem->GetFileName();
      dir_file_obj += "_";
      dir_file_obj += elem->GetObjName();
      TPair *p = dynamic_cast<TPair*>(bestElements.FindObject(dir_file_obj));
      if (p) {
         TDSetElement *prevelem = dynamic_cast<TDSetElement*>(p->Value());
         Long64_t entries = prevelem->GetFirst()+prevelem->GetNum();
         if (entries<elem->GetFirst()+elem->GetNum()) {
            bestElements.Remove(p);
            bestElements.Add(new TPair(p->Key(), elem));
            delete p;
         }
      } else {
         TNamed* named = new TNamed(dir_file_obj, dir_file_obj);
         namedHolder.Add(named);
         bestElements.Add(new TPair(named, elem));
      }
   }

   TIter nextElem(GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextElem())) {
      if (!elem->GetValid()) {
         TString dir_file_obj = elem->GetDirectory();
         dir_file_obj += "_";
         dir_file_obj += elem->GetFileName();
         dir_file_obj += "_";
         dir_file_obj += elem->GetObjName();
         if (TPair *p = dynamic_cast<TPair*>(bestElements.FindObject(dir_file_obj))) {
            TDSetElement* validelem = dynamic_cast<TDSetElement*>(p->Value());
            elem->Validate(validelem);
         }
      }
   }
}

//
// To handle requests coming from version 3 client / masters we need
// a special streamer
//______________________________________________________________________________
void TDSetElement::Streamer(TBuffer &R__b)
{
   // Stream an object of class TDSetElement.

   if (R__b.IsReading()) {
      UInt_t R__s, R__c;
      Version_t R__v = R__b.ReadVersion(&R__s, &R__c);
      ResetBit(kWriteV3);
      if (R__v > 4) {
         R__b.ReadClassBuffer(TDSetElement::Class(), this, R__v, R__s, R__c);
      } else {
         // For version 3 client / masters we need a special streamer
         SetBit(kWriteV3);
         if (R__v > 3) {
            TNamed::Streamer(R__b);
         } else {
            // Old versions were not deriving from TNamed and had the
            // file name and the object type name in the first two members
            TObject::Streamer(R__b);
            TString name, title;
            R__b >> name >> title;
            SetNameTitle(name, title);
         }
         // Now we read the standard part
         R__b >> fDirectory;
         R__b >> fFirst;
         R__b >> fNum;
         R__b >> fMsd;
         R__b >> fTDSetOffset;
         TEventList *evl;
         R__b >> evl;
         R__b >> fValid;
         R__b >> fEntries;

         // Special treatment waiting for proper retrieving of stl containers
         FriendsList_t *friends = new FriendsList_t;
         ((TStreamerInfo*)TClass::GetClass(typeid(FriendsList_t))->
            GetStreamerInfo())->ReadBuffer(R__b,(char**)&friends,0);
         if (friends) {
            // Convert friends to a TList (to be written)
            fFriends = new TList();
            fFriends->SetOwner();
            for (FriendsList_t::iterator i = friends->begin();
                 i != friends->end(); ++i) {
               TDSetElement *dse = (TDSetElement *) i->first->Clone();
               fFriends->Add(new TPair(dse, new TObjString(i->second.Data())));
            }
         }
         // the value for fIsTree (only older versions are sending it)
         Bool_t tmpIsTree;
         R__b >> tmpIsTree;
         R__b.CheckByteCount(R__s, R__c, TDSetElement::IsA());
      }
   } else {
      if (TestBit(kWriteV3)) {
         // For version 3 client / masters we need a special streamer
         R__b << Version_t(3);
         TObject::Streamer(R__b);
         R__b << TString(GetName());
         R__b << TString(GetTitle());
         R__b << fDirectory;
         R__b << fFirst;
         R__b << fNum;
         R__b << fMsd;
         R__b << fTDSetOffset;
         R__b << (TEventList *)0;
         R__b << fValid;
         R__b << fEntries;

         // Special treatment waiting for proper retrieving of stl containers
         FriendsList_t *friends = new FriendsList_t;
         if (fFriends) {
            TIter nxf(fFriends);
            TPair *p = 0;
            while ((p = (TPair *)nxf()))
               friends->push_back(std::make_pair((TDSetElement *)p->Key(),
                                   TString(((TObjString *)p->Value())->GetName())));
         }
         ((TStreamerInfo*)TClass::GetClass(typeid(FriendsList_t))->
            GetStreamerInfo())->WriteBuffer(R__b,(char *)friends,0);

         // Older versions had an unused boolean called fIsTree: we fill it
         // with its default value
         R__b << kFALSE;
      } else {
         R__b.WriteClassBuffer(TDSetElement::Class(),this);
      }
   }
}

//______________________________________________________________________________
void TDSet::Streamer(TBuffer &R__b)
{
   // Stream an object of class TDSet.

   if (R__b.IsReading()) {
      UInt_t R__s, R__c;
      Version_t R__v = R__b.ReadVersion(&R__s, &R__c);
      ResetBit(kWriteV3);
      if (R__v > 3) {
         R__b.ReadClassBuffer(TDSet::Class(), this, R__v, R__s, R__c);
      } else {
         // For version 3 client / masters we need a special streamer
         SetBit(kWriteV3);
         TNamed::Streamer(R__b);
         R__b >> fDir;
         R__b >> fType;
         R__b >> fObjName;
         TList elems;
         elems.Streamer(R__b);
         elems.SetOwner(kFALSE);
         if (elems.GetSize() > 0) {
            fElements = new THashList;
            fElements->SetOwner();
            TDSetElement *e = 0;
            TIter nxe(&elems);
            while ((e = (TDSetElement *)nxe())) {
               fElements->Add(e);
            }
         } else {
            fElements = 0;
         }
         R__b >> fIsTree;
      }
   } else {
      if (TestBit(kWriteV3)) {
         // For version 3 client / masters we need a special streamer
         R__b << Version_t(3);
         TNamed::Streamer(R__b);
         R__b << fDir;
         R__b << fType;
         R__b << fObjName;
         TList elems;
         if (fElements) {
            elems.SetOwner(kFALSE);
            if (fElements->GetSize() > 0) {
               TDSetElement *e = 0;
               TIter nxe(fElements);
               while ((e = (TDSetElement *)nxe()))
                  elems.Add(e);
            }
         }
         elems.Streamer(R__b);
         R__b << fIsTree;
      } else {
         R__b.WriteClassBuffer(TDSet::Class(),this);
      }
   }
}

//______________________________________________________________________________
void TDSet::SetWriteV3(Bool_t on)
{
   // Set/Reset the 'OldStreamer' bit in this instance and its elements.
   // Needed for backward compatibility in talking to old client / masters.

   if (on)
      SetBit(TDSet::kWriteV3);
   else
      ResetBit(TDSet::kWriteV3);
   // Loop over dataset elements
   TIter nxe(GetListOfElements());
   TObject *o = 0;
   while ((o = nxe()))
      if (on)
         o->SetBit(TDSetElement::kWriteV3);
      else
         o->ResetBit(TDSetElement::kWriteV3);
}

//______________________________________________________________________________
void TDSet::SetEntryList(TObject *aList)
{
   // Set entry (or event) list for this data set

   if (!aList)
      return;

   // Link the proper object
   TEventList *evl = 0;
   TEntryList *enl = dynamic_cast<TEntryList *>(aList);
   if (!enl)
      evl = dynamic_cast<TEventList *>(aList);
   if (!enl && !evl) {
      Error("SetEntryList", "type of input object must be either TEntryList "
                            "or TEventList (found: '%s' - do nothing", aList->ClassName());
      return;
   }

   // Action depends on the type
   fEntryList = (enl) ? enl : (TEntryList *)evl;

   // Done
   return;
}

//______________________________________________________________________________
void TDSet::SplitEntryList()
{
   // Splits the main entry (or event) list into sub-lists for the elements of
   // thet data set

   if (!fEntryList) {
      if (gDebug > 0)
         Info("SplitEntryList", "no entry- (or event-) list to split - do nothing");
      return;
   }

   // Action depend on type of list
   TEntryList *enl = dynamic_cast<TEntryList *>(fEntryList);
   if (enl) {
      // TEntryList
      TIter next(fElements);
      TDSetElement *el=0;
      TEntryList *sublist = 0;
      while ((el=(TDSetElement*)next())){
         sublist = enl->GetEntryList(el->GetObjName(), el->GetFileName());
         if (sublist){
            el->SetEntryList(sublist);
            el->SetNum(sublist->GetN());
         } else {
            sublist = new TEntryList("", "");
            el->SetEntryList(sublist);
            el->SetNum(0);
         }
      }
   } else {
      TEventList *evl = dynamic_cast<TEventList *>(fEntryList);
      if (evl) {
         // TEventList
         TIter next(fElements);
         TDSetElement *el, *prev;

         prev = dynamic_cast<TDSetElement*> (next());
         if (!prev)
            return;
         Long64_t low = prev->GetTDSetOffset();
         Long64_t high = low;
         Long64_t currPos = 0;
         do {
            el = dynamic_cast<TDSetElement*> (next());
            // kMaxLong64 means infinity
            high = (el == 0) ? kMaxLong64 : el->GetTDSetOffset();
#ifdef DEBUG
            while (currPos < evl->GetN() && evl->GetEntry(currPos) < low) {
               Error("SplitEntryList",
                     "TEventList: event outside of the range of any of the TDSetElements");
               currPos++;        // unnecessary check
            }
#endif
            TEventList* nevl = new TEventList();
            while (currPos < evl->GetN() && evl->GetEntry((Int_t)currPos) < high) {
               nevl->Enter(evl->GetEntry((Int_t)currPos) - low);
               currPos++;
            }
            prev->SetEntryList(nevl);
            prev->SetNum(nevl->GetN());
            low = high;
            prev = el;
         } while (el);
      }
   }
}
