// @(#)root/proof:$Id: TProofPlayer.h,v 1.41 2007/01/30 16:34:54 rdm Exp $
// Author: Fons Rademakers   15/03/07

/*************************************************************************
 * Copyright (C) 1995-2007, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "TVirtualProofPlayer.h"
#include "TPluginManager.h"
#include "TROOT.h"
#include "TError.h"


//______________________________________________________________________________
TVirtualProofPlayer *TVirtualProofPlayer::Create(const char *player,
                                                 TProof *pr, TSocket *s)
{
   // Create a PROOF player.

   TPluginHandler *h;
   TVirtualProofPlayer *p = 0;

   if (!player || !*player) {
      ::Error("TVirtualProofPlayer::Create", "player name missing");
      return 0;
   }

   if ((h = gROOT->GetPluginManager()->FindHandler("TVirtualProofPlayer", player))) {
      if (h->LoadPlugin() == -1)
         return 0;
      if (!strcmp(player, "slave"))
         p = (TVirtualProofPlayer *) h->ExecPlugin(1, s);
      else
         p = (TVirtualProofPlayer *) h->ExecPlugin(1, pr);
   }

   return p;
}
