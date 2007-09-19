// @(#)root/mathcore:$Id: Vector3Dfwd.h,v 1.3 2006/04/11 13:06:15 moneta Exp $
// Authors: W. Brown, M. Fischler, L. Moneta    2005  

 /**********************************************************************
  *                                                                    *
  * Copyright (c) 2005 , LCG ROOT MathLib Team                         *
  *                                                                    *
  *                                                                    *
  **********************************************************************/

// Header file Vector2Dfwd
// 
// Created by: Lorenzo Moneta  at Mon Apr 16 2007
// 
// 
#ifndef ROOT_Math_Vector2Dfwd 
#define ROOT_Math_Vector2Dfwd  1

// forward declarations of displacement vectors (Vectors) and type defs definitions

namespace ROOT { 

   namespace Math { 


      template<class CoordSystem, class Tag> class DisplacementVector2D; 

      template<typename T> class Cartesian2D;  
      template<typename T> class Polar2D;  

      class DefaultCoordinateSystemTag; 


      /**
         2D Vector based on the cartesian coordinates x,y in double precision
      */
      typedef DisplacementVector2D< Cartesian2D<double>, DefaultCoordinateSystemTag > XYVector; 
      typedef XYVector XYVectorD; 

      /**
         2D Vector based on the cartesian coordinates x,y,z in single precision
      */
      typedef DisplacementVector2D< Cartesian2D<float>, DefaultCoordinateSystemTag > XYVectorF; 


      /**
         2D Vector based on the polar coordinates rho, phi in double precision. 
      */
      typedef DisplacementVector2D< Polar2D<double>, DefaultCoordinateSystemTag > Polar2DVector; 
      typedef Polar2DVector Polar2DVectorD; 

      /**
         2D Vector based on the polar coordinates rho, phi in single precision. 
      */
      typedef DisplacementVector2D< Polar2D<float>, DefaultCoordinateSystemTag > Polar2DVectorF; 


    

   } // end namespace Math

} // end namespace ROOT


#endif /* ROOT_Math_Vector2Dfwd  */
