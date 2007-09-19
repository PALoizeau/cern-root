// @(#)root/pyroot:$Id: RootWrapper.cxx,v 1.53 2007/06/14 05:12:03 brun Exp $
// Author: Wim Lavrijsen, Apr 2004

// Bindings
#include "PyROOT.h"
#include "PyRootType.h"
#include "ObjectProxy.h"
#include "MethodProxy.h"
#include "PropertyProxy.h"
#include "RootWrapper.h"
#include "Pythonize.h"
#include "MethodHolder.h"
#include "ConstructorHolder.h"
#include "ClassMethodHolder.h"
#include "FunctionHolder.h"
#include "TSetItemHolder.h"
#include "MemoryRegulator.h"
#include "Utility.h"
#include "Adapters.h"

// ROOT
#include "TROOT.h"
#include "TSystem.h"
#include "TMethod.h"
#include "TMethodArg.h"
#include "TBaseClass.h"
#include "TInterpreter.h"
#include "TGlobal.h"
#include "DllImport.h"

// CINT
#include "Api.h"

// Reflex
#ifdef PYROOT_USE_REFLEX
#include "Reflex/Scope.h"
#include "Reflex/Base.h"
#include "Reflex/Member.h"
#include "Reflex/Object.h"
#endif

// Standard
#include <map>
#include <set>
#include <string>
#include <algorithm>
#include <vector>


//- data _______________________________________________________________________
R__EXTERN PyObject* gRootModule;

namespace {

// to prevent having to walk scopes, track python classes by ROOT class
   typedef std::map< void*, PyObject* > PyClassMap_t;
   PyClassMap_t gPyClasses;

// helper for creating new ROOT python types
   PyObject* CreateNewROOTPythonClass( const std::string& name, PyObject* pybases )
   {
      Py_XINCREF( pybases );
      if ( ! pybases ) {
         pybases = PyTuple_New( 1 );
         Py_INCREF( &PyROOT::ObjectProxy_Type );
         PyTuple_SET_ITEM( pybases, 0, (PyObject*)&PyROOT::ObjectProxy_Type );
      }

      PyObject* pymetabases = PyTuple_New( PyTuple_GET_SIZE( pybases ) );
      for ( int i = 0; i < PyTuple_GET_SIZE( pybases ); ++i ) {
         PyObject* btype = (PyObject*)PyTuple_GetItem( pybases, i )->ob_type;
         Py_INCREF( btype );
         PyTuple_SET_ITEM( pymetabases, i, btype );
      }

      PyObject* args = Py_BuildValue( (char*)"sO{}", (name+"_meta").c_str(), pymetabases );
      Py_DECREF( pymetabases );

      PyObject* pymeta = PyType_Type.tp_new( &PyROOT::PyRootType_Type, args, NULL );
      if ( ! pymeta )
         PyErr_Print();
      Py_DECREF( args );

      args = Py_BuildValue( (char*)"sO{}", name.c_str(), pybases );
      PyObject* pyclass = PyType_Type.tp_new( (PyTypeObject*)pymeta, args, NULL );
      Py_DECREF( args );
      Py_DECREF( pymeta );

      Py_DECREF( pybases );

      return pyclass;
   }

// helper to split between CINT and Reflex
   Long_t GetDataMemberAddress( TClass* klass, TDataMember* mb )
   {
      Long_t offset = 0;
      G__DataMemberInfo dmi = klass->GetClassInfo()->GetDataMember( mb->GetName(), &offset );
      return dmi.Offset();
   }

#ifdef PYROOT_USE_REFLEX
   Long_t GetDataMemberAddress( const ROOT::Reflex::Scope&, const ROOT::Reflex::Member& mb )
   {
      return (Long_t)mb.Offset();
   }
#endif

} // unnamed namespace


//- helpers --------------------------------------------------------------------
namespace {

   inline void AddToScope( const char* label, TObject* obj, TClass* klass )
   {
      PyModule_AddObject( gRootModule, const_cast< char* >( label ),
         PyROOT::BindRootObject( obj, klass ) );
   }

   std::set< std::string > gSTLTypes, gLoadedSTLTypes;
   struct InitSTLTypes_t {
      InitSTLTypes_t()
      {
         const char* stlTypes[] = { "complex", "exception",
            "deque", "list", "queue", "stack", "vector",
            "map", "multimap", "set", "multiset" };
         std::string nss = "std::";
         for ( int i = 0; i < int(sizeof(stlTypes)/sizeof(stlTypes[0])); ++i ) {
            gSTLTypes.insert( stlTypes[ i ] );
            gSTLTypes.insert( nss + stlTypes[ i ] );
         }
         gLoadedSTLTypes.insert( "vector" );
      }
   } initSTLTypes_;

   Bool_t LoadDictionaryForSTLType( const std::string& tname, void* klass )
   {
   // if name is of a known STL class, load the appropriate CINT dll(s), always reset klass

      std::string sub = tname.substr( 0, tname.find( "<" ) );
      if ( gSTLTypes.find( sub ) != gSTLTypes.end() ) {
      // removal is required or the dictionary can't be updated properly
         if ( klass != 0 )
            TClass::RemoveClass( (TClass*)klass );

         Bool_t result = kTRUE;

      // make sure to only load once
         if ( gLoadedSTLTypes.find( sub ) == gLoadedSTLTypes.end() ) {

         // strip std:: part as needed to form proper file name
            if ( sub.substr( 0, 5 ) == "std::" )
               sub = sub.substr( 5, std::string::npos );

         // attempt to load file (may not be built and therefore unavailable)
            int loadOk = G__loadfile( (sub+".dll").c_str() );
            if ( 0 <= loadOk ) {
            // special case for map and multimap, which are spread over 2 files
               if ( sub == "map" || sub == "multimap" ) {
                  loadOk = G__loadfile( (sub+"2.dll").c_str() );
               }

            // success; prevent second attempt to load by erasing name
               gLoadedSTLTypes.insert( sub );
               gLoadedSTLTypes.insert( "std::" + sub );
            }

         // on success, synchronize gROOT and CINT, otherwise warn user of doom
            if ( 0 <= loadOk ) {
               gInterpreter->UpdateListOfTypes();
            } else {
               PyErr_Warn( PyExc_RuntimeWarning,
                  const_cast< char* >( ( "could not load dict lib for " + sub ).c_str() ) );
               result = kFALSE;
            }

         }

      // verify that CINT understands the class, then add it for gROOT
         if ( G__ClassInfo( tname.c_str() ).IsValid() ) {
            TClass* cl = new TClass( tname.c_str() );
            TClass::AddClass( cl );
         }

         return result;
      }

   // this point is only reached if this is not an STL class, but that's ok
      return kTRUE;
   }

} // unnamed namespace


//- public functions ---------------------------------------------------------
void PyROOT::InitRoot()
{
// setup interpreter locks to allow for threading in ROOT
   PyEval_InitThreads();

// memory management
   static TMemoryRegulator m;
   gROOT->GetListOfCleanups()->Add( &m );

// bind ROOT globals that are needed in ROOT.py
   AddToScope( "gROOT", gROOT, gROOT->IsA() );
   AddToScope( "gSystem", gSystem, gSystem->IsA() );
   AddToScope( "gInterpreter", gInterpreter, gInterpreter->IsA() );
}

//____________________________________________________________________________
template< class T, class B, class M >
int PyROOT::BuildRootClassDict( const T& klass, PyObject* pyclass ) {
// get the unscoped class name
   std::string clName = klass.Name();

// some properties that'll affect building the dictionary
   Bool_t isNamespace = klass.IsNamespace();
   Bool_t hasConstructor = kFALSE;

// load all public methods and data members
   typedef std::vector< PyCallable* > Callables_t;
   typedef std::map< std::string, Callables_t > CallableCache_t;
   CallableCache_t cache;

   const size_t nMethods = klass.FunctionMemberSize();
   for ( size_t inm = 0; inm < nMethods; ++inm ) {
      const M& method = klass.FunctionMemberAt( inm );

   // special case tracker
      Bool_t setupSetItem = kFALSE;

   // retrieve method name
      std::string mtName = method.Name();

   // filter empty names (happens for namespaces, is bug?)
      if ( mtName == "" )
         continue;

   // filter C++ destructors
      if ( mtName[0] == '~' )
         continue;

   // translate operators
      if ( 8 < mtName.size() && mtName.substr( 0, 8 ) == "operator" ) {
         std::string op = mtName.substr( 8, std::string::npos );

      // filter assignment operator for later use
         if ( op == "=" ) {
            continue;
         }

      // map C++ operator to python equivalent, or made up name if no equivalent exists
         Utility::TC2POperatorMapping_t::iterator pop = Utility::gC2POperatorMapping.find( op );
         if ( pop != Utility::gC2POperatorMapping.end() ) {
            mtName = pop->second;
         } else if ( op == "[]" || op == "()" ) {        // index or call
            mtName = op == "()" ? "__call__" : "__getitem__";

         // operator[]/() returning a reference type will be used for __setitem__
            std::string cpd = Utility::Compound(
               method.TypeOf().ReturnType().Name( ROOT::Reflex::Q | ROOT::Reflex::S ) );
            if ( cpd[ cpd.size() - 1 ] == '&' )
               setupSetItem = kTRUE;
         } else if ( op == "*" ) {
            if ( method.FunctionParameterSize() == 0 )   // dereference
               mtName = "__deref__";
            else                                         // multiplier (is python equivalent)
               mtName = "__mul__";
         } else if ( op == "++" ) {
            if ( method.FunctionParameterSize() == 0 )   // prefix increment
               mtName = "__preinc__"; 
            else                                         // postfix increment
               mtName = "__postinc__";
         } else if ( op == "--" ) {
            if ( method.FunctionParameterSize() == 0 )   // prefix decrement
               mtName = "__predec__";
            else                                         // postfix decrement
               mtName = "__postdec__";
         } else if ( op == "->" ) {                      // class member access
             mtName = "__follow__";
         } else {
            continue;                                    // not handled (new, delete, etc.)
         }
      }

   // decide on method type: member or static (which includes globals)
      Bool_t isStatic = isNamespace || method.IsStatic();

   // public methods are normally visible, private methods are mangled python-wise
   // note the overload implications which are name based, and note that rootcint
   // does not create the interface methods for private/protected methods ...
      if ( ! method.IsPublic() ) {
         if ( mtName == clName )             // don't expose private ctors
            continue;
         else                                // mangle private methods
            mtName = "_" + clName + "__" + mtName;
      }

   // construct the holder
      PyCallable* pycall = 0;
      if ( isStatic == kTRUE )               // class method
         pycall = new TClassMethodHolder< T, M >( klass, method );
      else if ( mtName == clName ) {         // constructor
         pycall = new TConstructorHolder< T, M >( klass, method );
         mtName = "__init__";
         hasConstructor = kTRUE;
      } else                                 // member function
         pycall = new TMethodHolder< T, M >( klass, method );

   // lookup method dispatcher and store method
      Callables_t& md = (*(cache.insert(
         std::make_pair( mtName, Callables_t() ) ).first)).second;
      md.push_back( pycall );

   // special case for operator[]/() that returns by ref, use for getitem/call and setitem
      if ( setupSetItem ) {
         Callables_t& setitem = (*(cache.insert(
            std::make_pair( std::string( "__setitem__" ), Callables_t() ) ).first)).second;
         setitem.push_back( new TSetItemHolder< T, M >( klass, method ) );
      }
   }

// add a pseudo-default ctor, if none defined
   if ( ! isNamespace && ! hasConstructor )
      cache[ "__init__" ].push_back( new TConstructorHolder< T, M >( klass ) );

// add the methods to the class dictionary
   for ( CallableCache_t::iterator imd = cache.begin(); imd != cache.end(); ++imd ) {
      MethodProxy* method = MethodProxy_New( imd->first, imd->second );
      PyObject_SetAttrString(
         pyclass, const_cast< char* >( method->GetName().c_str() ), (PyObject*)method );
      Py_DECREF( method );
   }

// collect data members
   const size_t nDataMembers = klass.DataMemberSize();
   for ( size_t ind = 0; ind < nDataMembers; ++ind ) {
      const M& mb = klass.DataMemberAt( ind );

   // allow only public members
      if ( ! mb.IsPublic() )
         continue;

   // enums (static enums are the defined values, non-static are data members, i.e. properties)
      if ( mb.TypeOf().IsEnum() && mb.IsStatic() ) {
         PyObject* val = PyInt_FromLong( *((Long_t*)GetDataMemberAddress( klass, mb ) ) );
         PyObject_SetAttrString( pyclass, const_cast<char*>(mb.Name().c_str()), val );
         Py_DECREF( val );

   // properties (aka public data members)
      } else {
         PropertyProxy* property = PropertyProxy_New( mb );

      // allow access at the instance level
         PyObject_SetAttrString( pyclass,
            const_cast< char* >( property->GetName().c_str() ), (PyObject*)property );

         if ( mb.IsStatic() ) {
         // allow access at the class level (always add after setting instance level)
            PyObject_SetAttrString( (PyObject*)pyclass->ob_type,
               const_cast< char* >( property->GetName().c_str() ), (PyObject*)property );
         }

         Py_DECREF( property );
      }
   }

// all ok, done
   return 0;
}

//____________________________________________________________________________
template< class T, class B, class M >
PyObject* PyROOT::BuildRootClassBases( const T& klass )
{
   size_t nbases = klass.BaseSize();

// collect bases while removing duplicates
   std::vector< std::string > uqb;
   uqb.reserve( nbases );

   for ( size_t inb = 0; inb < nbases; ++inb ) {
      const B& base = klass.BaseAt( inb );
      std::string name = base.Name();
      if ( std::find( uqb.begin(), uqb.end(), name ) == uqb.end() ) {
         uqb.push_back( name );
      }
   }

// allocate a tuple for the base classes, special case for first base
   nbases = uqb.size();

   PyObject* pybases = PyTuple_New( nbases ? nbases : 1 );
   if ( ! pybases )
      return 0;

// build all the bases
   if ( nbases == 0 ) {
      Py_INCREF( &ObjectProxy_Type );
      PyTuple_SET_ITEM( pybases, 0, (PyObject*)&ObjectProxy_Type );
   } else {
      for ( std::vector< std::string >::size_type ibase = 0; ibase < nbases; ++ibase ) {
         PyObject* pyclass = MakeRootClassFromString< T, B, M >( uqb[ ibase ] );
         if ( ! pyclass ) {
            Py_DECREF( pybases );
            return 0;
         }

         PyTuple_SET_ITEM( pybases, ibase, pyclass );
      }
   }

   return pybases;
}

#ifdef PYROOT_USE_REFLEX
template PyObject* PyROOT::BuildRootClassBases< \
   ROOT::Reflex::Scope, ROOT::Reflex::Base, ROOT::Reflex::Member >( const ROOT::Reflex::Scope& );
#endif


//____________________________________________________________________________
PyObject* PyROOT::MakeRootClass( PyObject*, PyObject* args )
{
   std::string cname = PyString_AsString( PyTuple_GetItem( args, 0 ) );

   if ( PyErr_Occurred() )
      return 0;

   return MakeRootClassFromString< TScopeAdapter, TBaseAdapter, TMemberAdapter >( cname );
}

//____________________________________________________________________________
PyObject* PyROOT::MakeRootClassFromType( TClass* klass )
{
// locate class by full name, if possible to prevent parsing scopes/templates anew
   PyClassMap_t::iterator pci = gPyClasses.find( (void*)klass );
   if ( pci != gPyClasses.end() ) {
      PyObject* pyclass = PyWeakref_GetObject( pci->second );
      if ( pyclass ) {
         Py_INCREF( pyclass );
         return pyclass;
      }
   }

// still here ... pyclass not created or no longer valid, need full parsing
   return MakeRootClassFromString< TScopeAdapter, TBaseAdapter, TMemberAdapter >( klass->GetName() );
}

//____________________________________________________________________________
template< class T, class B, class M >
PyObject* PyROOT::MakeRootClassFromString( const std::string& fullname, PyObject* scope )
{
// force building of the class if a scope is specified (prevents loops)
   Bool_t force = scope != 0;

// working copy
   std::string name = fullname;

// determine scope name, if a python scope has been given
   std::string scName = "";
   if ( scope ) {
      PyObject* pyscope = PyObject_GetAttrString( scope, const_cast< char* >( "__name__" ) );
      if ( ! pyscope ) {
         PyErr_Format( PyExc_SystemError, "given scope has no name for %s", name.c_str() );
         return 0;
      }

   // should be a string
      scName = PyString_AsString( pyscope );
      Py_DECREF( pyscope );
      if ( PyErr_Occurred() )
         return 0;

   // work with scope from now on
      Py_INCREF( scope );
   }

// retrieve ROOT class (this verifies name)
   const std::string& lookup = scope ? (scName+"::"+name) : name;
   T klass = T::ByName( lookup );
   if ( ! (bool)klass || ( (bool)klass && klass.FunctionMemberSize() == 0 ) ) {
   // special action for STL classes to enforce loading dict lib
      LoadDictionaryForSTLType( name, klass.Id() );

   // lookup again, if this was an STL class, we (may) now have a full dictionary
      klass = T::ByName( lookup );
   }

   if ( ! (bool)klass && G__defined_templateclass( const_cast< char* >( lookup.c_str() ) ) ) {
   // a "naked" templated class is requested: return callable proxy for instantiations
      PyObject* pytcl = PyObject_GetAttrString( gRootModule, const_cast< char* >( "Template" ) );
      PyObject* pytemplate = PyObject_CallFunction(
         pytcl, const_cast< char* >( "s" ), const_cast< char* >( lookup.c_str() ) );
      Py_DECREF( pytcl );

   // cache the result
      PyObject_SetAttrString( scope ? scope : gRootModule, (char*)name.c_str(), pytemplate );

   // done, next step should be a call into this template
      Py_XDECREF( scope );
      return pytemplate;
   }

   if ( ! (bool)klass && G__defined_tagname( lookup.c_str(), 2 ) != -1 ) {
   // an unloaded namespace is requested
      PyObject* pyns = CreateNewROOTPythonClass( lookup, NULL );

   // cache the result
      PyObject_SetAttrString( scope ? scope : gRootModule, (char*)name.c_str(), pyns );

   // done, next step should be a lookup into this namespace
      Py_XDECREF( scope );
      return pyns;
   }

   if ( ! (bool)klass ) {   // if so, all options have been exhausted: it doesn't exist as such
      if ( ! scope && fullname.find( "ROOT::" ) == std::string::npos ) { // not already in ROOT::
      // final attempt, for convenience, the "ROOT" namespace isn't required, try again ...
         PyObject* rtns = PyObject_GetAttrString( gRootModule, const_cast< char* >( "ROOT" ) );
         PyObject* pyclass = PyObject_GetAttrString( rtns, (char*)fullname.c_str() );
         Py_DECREF( rtns );
         return pyclass;
      }

      PyErr_Format( PyExc_TypeError, "requested class \'%s\' does not exist", lookup.c_str() );
      Py_XDECREF( scope );
      return 0;
   }

// locate the scope, if necessary, for building the class if not specified
   if ( ! scope ) {
   // need to deal with template paremeters that can have scopes themselves
      Int_t tpl_open = 0;
      std::string::size_type last = 0;
      for ( std::string::size_type pos = 0; pos < name.size(); ++pos ) {
         std::string::value_type c = name[ pos ];

      // count '<' and '>' to be able to skip template contents
         if ( c == '<' )
            ++tpl_open;
         else if ( c == '>' )
            --tpl_open;

      // by only checking for "::" the last part (class name) is dropped
         else if ( tpl_open == 0 &&\
              c == ':' && pos+1 < name.size() && name[ pos+1 ] == ':' ) {
         // found a new scope part
            std::string part = name.substr( last, pos-last );

            PyObject* next = PyObject_GetAttrString(
               scope ? scope : gRootModule, const_cast< char* >( part.c_str() ) );

            if ( ! next ) {           // lookup failed, try to create it
               PyErr_Clear();
               next = MakeRootClassFromString< T, B, M >( part, scope );
            }
            Py_XDECREF( scope );

            if ( ! next )             // create failed, give up
               return 0;

         // found scope part
            scope = next;

         // done with part (note that pos is moved one ahead here)
            last = pos+2; ++pos;
         }

      }
   }

// use global scope if no inner scope found
   if ( ! scope ) {
      scope = gRootModule;
      Py_INCREF( scope );
   }

// use actual class name for binding
   std::string actual = klass.Name( ROOT::Reflex::FINAL );

// first try to retrieve an existing class representation
   PyObject* pyactual = PyString_FromString( actual.c_str() );
   PyObject* pyclass = force ? 0 : PyObject_GetAttr( scope, pyactual );

   Bool_t bClassFound = pyclass ? kTRUE : kFALSE;

// build if the class does not yet exist
   if ( ! pyclass ) {
   // ignore error generated from the failed lookup
      PyErr_Clear();

   // construct the base classes
      PyObject* pybases = BuildRootClassBases< T, B, M >( klass );
      if ( pybases != 0 ) {
      // create a fresh Python class, given bases, name, and empty dictionary
         pyclass = CreateNewROOTPythonClass( klass.Name( ROOT::Reflex::SCOPED ), pybases );
         Py_DECREF( pybases );
      }

   // fill the dictionary, if successful
      if ( pyclass != 0 ) {
         if ( BuildRootClassDict< T, B, M >( klass, pyclass ) != 0 ) {
         // something failed in building the dictionary
            Py_DECREF( pyclass );
            pyclass = 0;
         } else
            PyObject_SetAttr( scope, pyactual, pyclass );
      }
   }

   if ( pyclass && name != actual )     // class exists, but is typedef-ed: simply map reference
      PyObject_SetAttrString( scope, const_cast< char* >( name.c_str() ), pyclass );

   Py_DECREF( pyactual );
   Py_DECREF( scope );

   if ( ! bClassFound ) {               // add python-style features to newly minted classes
      if ( ! Pythonize( pyclass, klass.Name() ) ) {
         Py_XDECREF( pyclass );
         pyclass = 0;
      }
   }

   if ( pyclass )                      // store a ref from ROOT TClass to new python class
      gPyClasses[ klass.Id() ] = PyWeakref_NewRef( pyclass, NULL );

// all done
   return pyclass;
}

#ifdef PYROOT_USE_REFLEX
template PyObject* PyROOT::MakeRootClassFromString< ROOT::Reflex::Scope,\
   ROOT::Reflex::Base, ROOT::Reflex::Member >( const std::string&, PyObject* scope );
#endif

//____________________________________________________________________________
PyObject* PyROOT::GetRootGlobal( PyObject*, PyObject* args )
{
// get the requested name
   std::string ename = PyString_AsString( PyTuple_GetItem( args, 0 ) );

   if ( PyErr_Occurred() )
      return 0;

   return GetRootGlobalFromString( ename );
}

//____________________________________________________________________________
PyObject* PyROOT::GetRootGlobalFromString( const std::string& name )
{
// try named global variable/enum (first ROOT, then CINT: sync is too slow)
   TGlobal* gb = (TGlobal*)gROOT->GetListOfGlobals( kFALSE )->FindObject( name.c_str() );
   if ( gb ) return BindRootGlobal( gb );

   G__DataMemberInfo dt;
   while ( dt.Next() ) {
      if ( dt.IsValid() && dt.Name() == name ) {
         TGlobal gbl = TGlobal( new G__DataMemberInfo( dt ) );
         return BindRootGlobal( &gbl );
      }
   }

// still here ... try functions (first ROOT, then CINT: sync is too slow)
   TFunction* func =
      (TFunction*)gROOT->GetListOfGlobalFunctions( kFALSE )->FindObject( name.c_str() );
   if ( func )
      return (PyObject*)MethodProxy_New( name,
                new TFunctionHolder< TScopeAdapter, TMemberAdapter >( func ) );

   std::vector< PyCallable* > overloads;
   G__MethodInfo mt;
   while ( mt.Next() ) {
      if ( mt.IsValid() && mt.Name() == name ) {
      // add to list of globals (same as synchronization would do for all funcs)
         TFunction* func = new TFunction( new G__MethodInfo( mt ) );
         gROOT->GetListOfGlobalFunctions()->Add( func );

         overloads.push_back( new TFunctionHolder< TScopeAdapter, TMemberAdapter >( func ) );
      }
   }

   if ( ! overloads.empty() )
      return (PyObject*)MethodProxy_New( name, overloads );

// nothing found
   PyErr_Format( PyExc_LookupError, "no such global: %s", name.c_str() );
   return 0;
}

//____________________________________________________________________________
PyObject* PyROOT::BindRootObjectNoCast( void* address, TClass* klass, Bool_t isRef ) {
// only known or knowable objects will be bound (null object is ok)
   if ( ! klass ) {
      PyErr_SetString( PyExc_TypeError, "attempt to bind ROOT object w/o class" );
      return 0;
   }

// retrieve python class
   PyObject* pyclass = MakeRootClassFromType( klass );
   if ( ! pyclass )
      return 0;                    // error has been set in MakeRootClass

// instantiate an object of this class
   PyObject* args = PyTuple_New(0);
   ObjectProxy* pyobj =
      (ObjectProxy*)((PyTypeObject*)pyclass)->tp_new( (PyTypeObject*)pyclass, args, NULL );
   Py_DECREF( args );
   Py_DECREF( pyclass );

// bind, register and return if successful
   if ( pyobj != 0 ) {
   // fill proxy values
      if ( ! isRef )
         pyobj->Set( address, klass );
      else
         pyobj->Set( (void**)address, klass );
   }

// successful completion
   return (PyObject*)pyobj;
}

//____________________________________________________________________________
PyObject* PyROOT::BindRootObject( void* address, TClass* klass, Bool_t isRef )
{
// for safety (None can't be used as NULL pointer)
   if ( ! address ) {
      Py_INCREF( Py_None );
      return Py_None;
   }

// only known or knowable objects will be bound
   if ( ! klass ) {
      PyErr_SetString( PyExc_TypeError, "attempt to bind ROOT object w/o class" );
      return 0;
   }

// upgrade to real class for object returns
   if ( ! isRef ) {
      TClass* clActual = klass->GetActualClass( address );
      if ( clActual && klass != clActual ) {
       // root/meta base class offset fails in the case of virtual inheritance
       //   Long_t offset = clActual->GetBaseClassOffset( klass );
         Long_t offset;
         if (klass->GetClassInfo() &&  clActual->GetClassInfo()) {
            offset = G__isanybase(klass->GetClassInfo()->Tagnum(), clActual->GetClassInfo()->Tagnum(), (Long_t)address );
         } else {
            offset = clActual->GetBaseClassOffset( klass ); 
         }
         (Long_t&)address -= offset;
         klass = clActual;
      }
   }

// obtain pointer to TObject base class (if possible) for memory mgmt
   TObject* object = klass->IsTObject() ? ((TObject*)( isRef ? *((void**)address) : address )) : 0;
   if ( ! isRef && object ) {
      object = (TObject*)klass->DynamicCast( TObject::Class(), object );

   // use the old reference if the object already exists
      PyObject* oldPyObject = TMemoryRegulator::RetrieveObject( object );
      if ( oldPyObject )
         return oldPyObject;
   }

// actual binding
   ObjectProxy* pyobj = (ObjectProxy*)BindRootObjectNoCast( address, klass, isRef );

// memory management, for TObject's only
   if ( object )
      TMemoryRegulator::RegisterObject( pyobj, object );

// completion (returned object may be zero w/ a python exception set)
   return (PyObject*)pyobj;
}

//____________________________________________________________________________
PyObject* PyROOT::BindRootGlobal( TGlobal* gbl )
{
// gbl == 0 means global does not exist (rather than gbl is NULL pointer)
   if ( ! gbl ) {
      Py_INCREF( Py_None );
      return Py_None;
   }

// determine type and cast as appropriate
   TClass* klass = TClass::GetClass( gbl->GetTypeName() );
   if ( klass != 0 ) {
      if ( Utility::Compound( gbl->GetFullTypeName() ) != "" )
         return BindRootObject( (void*)gbl->GetAddress(), klass, kTRUE );

      return BindRootObject( (void*)gbl->GetAddress(), klass );
   }

   if ( gbl->GetAddress() &&       // check for enums (which are const, not properties)
        ( G__TypeInfo( gbl->GetTypeName() ).Property() & G__BIT_ISENUM ) ) {
      return PyInt_FromLong( *((int*)gbl->GetAddress()) );
   }

// for built-in types, to ensure setability
   return (PyObject*)PropertyProxy_New< TGlobal* >( gbl );
}
