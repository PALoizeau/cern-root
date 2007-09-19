// @(#)root/minuit2:$Id: TFitterFumili.h,v 1.3 2006/07/03 15:48:06 moneta Exp $
// Author: L. Moneta    10/2005  

/**********************************************************************
 *                                                                    *
 * Copyright (c) 2005 ROOT Foundation,  CERN/PH-SFT                   *
 *                                                                    *
 **********************************************************************/

#ifndef ROOT_TFitterFumili_H_
#define ROOT_TFitterFumili_H_


#ifndef ROOT_TVirtualFitter
#include "TVirtualFitter.h"
#endif

#include "TFitterMinuit.h"

/**
   TVirtualFitter implementation for new Fumili  
*/


class TFitterFumili : public TFitterMinuit {
   
public:

   TFitterFumili();

   TFitterFumili(Int_t maxpar);

   virtual ~TFitterFumili() { } 

public:

   //inherited interface
   virtual Double_t  Chisquare(Int_t npar, Double_t *params) const;


   //virtual FunctionMinimum Minimize(  int nfcn = 0, double edmval = 0.1) const;


  

protected: 

   void CreateMinimizer(EMinimizerType ); 

   void CreateChi2FCN(); 

   void CreateChi2ExtendedFCN(); 

   void CreateUnbinLikelihoodFCN() {}

   void CreateBinLikelihoodFCN();
   
private:

  

   ClassDef(TFitterFumili,1) // Fitter based on new Fumili method (Fumili2)
      };

R__EXTERN TFitterFumili* gFumili2;


#endif //ROOT_TFitterFumili_H_
