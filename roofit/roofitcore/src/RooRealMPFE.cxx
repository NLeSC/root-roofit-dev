/*****************************************************************************
 * Project: RooFit                                                           *
 * Package: RooFitCore                                                       *
 * @(#)root/roofitcore:$Id$
 * Authors:                                                                  *
 *   WV, Wouter Verkerke, UC Santa Barbara, verkerke@slac.stanford.edu       *
 *   DK, David Kirkby,    UC Irvine,         dkirkby@uci.edu                 *
 *                                                                           *
 * Copyright (c) 2000-2005, Regents of the University of California          *
 *                          and Stanford University. All rights reserved.    *
 *                                                                           *
 * Redistribution and use in source and binary forms,                        *
 * with or without modification, are permitted according to the terms        *
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)             *
 *****************************************************************************/

/**
\file RooRealMPFE.cxx
\class RooRealMPFE
\ingroup Roofitcore

RooRealMPFE is the multi-processor front-end for parallel calculation
of RooAbsReal objects. Each RooRealMPFE forks a process that calculates
the value of the proxies RooAbsReal object. The (re)calculation of
the proxied object is started asynchronously with the calculate() option.
A subsequent call to getVal() will return the calculated value when available
If the calculation is still in progress when getVal() is called it blocks
the calling process until the calculation is done. The forked calculation process
is terminated when the front-end object is deleted
Simple use demonstration

~~~{.cpp}
RooAbsReal* slowFunc ;

Double_t val = slowFunc->getVal() // Evaluate slowFunc in current process

RooRealMPFE mpfe("mpfe","frontend to slowFunc",*slowFunc) ;
mpfe.calculate() ;           // Start calculation of slow-func in remote process
                             // .. do other stuff here ..
Double_t val = mpfe.getVal() // Wait for remote calculation to finish and retrieve value
~~~

For general multiprocessing in ROOT, please refer to the TProcessExecutor class.

**/

#include "Riostream.h"
#include "RooFit.h"

#ifndef _WIN32
#include "BidirMMapPipe.h"
#endif

#include <cstdlib>
#include <sstream>
#include "RooRealMPFE.h"
#include "RooArgSet.h"
#include "RooAbsCategory.h"
#include "RooRealVar.h"
#include "RooCategory.h"
#include "RooMPSentinel.h"
#include "RooMsgService.h"
#include "RooNLLVar.h"
#include "RooTrace.h"
#include "RooConstVar.h"
#include "RooRealIntegral.h"

// for _initNumIntSet
#include "RooAbsData.h"

#include "TSystem.h"

// for cpu affinity
#if !defined(__APPLE__) && !defined(_WIN32)
#include <sched.h>
#endif

#include <chrono>
#include <fstream>

RooMPSentinel RooRealMPFE::_sentinel ;

// FIXME: remove this include?
#include "../../../graf3d/ftgl/inc/FTGL.h"

// timing
#include "RooTimer.h"
// getpid and getppid:
#include "unistd.h"

using namespace std;
using namespace RooFit;

ClassImp(RooRealMPFE)
  ;


////////////////////////////////////////////////////////////////////////////////
/// Construct front-end object for object 'arg' whose evaluation will be calculated
/// asynchronously in a separate process. If calcInline is true the value of 'arg'
/// is calculate synchronously in the current process.

RooRealMPFE::RooRealMPFE(const char *name, const char *title, RooAbsReal& arg, Bool_t calcInline) :
  RooAbsReal(name,title),
  _state(Initialize),
  _arg("arg","arg",this,arg),
  _vars("vars","vars",this),
  _calcInProgress(kFALSE),
  _verboseClient(kFALSE),
  _verboseServer(kFALSE),
  _inlineMode(calcInline),
  _remoteEvalErrorLoggingState(RooAbsReal::PrintErrors),
  _pipe(0),
  _updateMaster(0),
  _retrieveDispatched(kFALSE), _evalCarry(0.)
{
#ifdef _WIN32
  _inlineMode = kTRUE;
#endif
  initVars() ;
  _sentinel.add(*this) ;

}



////////////////////////////////////////////////////////////////////////////////
/// Copy constructor. Initializes in clean state so that upon eval
/// this instance will create its own server processes

RooRealMPFE::RooRealMPFE(const RooRealMPFE& other, const char* name) :
  RooAbsReal(other, name),
  _state(Initialize),
  _arg("arg",this,other._arg),
  _vars("vars",this,other._vars),
  _calcInProgress(kFALSE),
  _verboseClient(other._verboseClient),
  _verboseServer(other._verboseServer),
  _inlineMode(other._inlineMode),
  _forceCalc(other._forceCalc),
  _remoteEvalErrorLoggingState(other._remoteEvalErrorLoggingState),
  _pipe(0),
  _updateMaster(0),
  _retrieveDispatched(kFALSE), _evalCarry(other._evalCarry)
{
  initVars() ;
  _sentinel.add(*this) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Destructor

RooRealMPFE::~RooRealMPFE()
{
  if (_state==Client) standby();
  _sentinel.remove(*this);

  delete _components;
}



////////////////////////////////////////////////////////////////////////////////
/// Initialize list of variables of front-end argument 'arg'

void RooRealMPFE::initVars()
{
  // Empty current lists
  _vars.removeAll() ;
  _saveVars.removeAll() ;

  // Retrieve non-constant parameters
  RooArgSet* vars = _arg.arg().getParameters(RooArgSet()) ;
  //RooArgSet* ncVars = (RooArgSet*) vars->selectByAttrib("Constant",kFALSE) ;
  RooArgList varList(*vars) ;

  // Save in lists
  _vars.add(varList) ;
  _saveVars.addClone(varList) ;
  _valueChanged.resize(_vars.getSize()) ;
  _constChanged.resize(_vars.getSize()) ;

  // Force next calculation
  _forceCalc = kTRUE ;

  delete vars ;
  //delete ncVars ;
}

Double_t RooRealMPFE::getCarry() const
{
  if (_inlineMode) {
    RooAbsTestStatistic* tmp = dynamic_cast<RooAbsTestStatistic*>(_arg.absArg());
    if (tmp) return tmp->getCarry();
    else return 0.;
  } else {
    return _evalCarry;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// Initialize the remote process and message passing
/// pipes between current process and remote process

void RooRealMPFE::initialize()
{
  // Trivial case: Inline mode
  if (_inlineMode) {
    _state = Inline ;
    return ;
  }

#ifndef _WIN32
  // Clear eval error log prior to forking
  // to avoid confusions...
  clearEvalErrorLog() ;
  // Fork server process and setup IPC
  _pipe = new BidirMMapPipe();

  if (_pipe->isChild()) {
    // Start server loop
//    RooTrace::callgrind_zero() ;
    _state = Server ;
    serverLoop();

    // Kill server at end of service
    if (_verboseServer) ccoutD(Minimization) << "RooRealMPFE::initialize(" <<
	GetName() << ") server process terminating" << endl ;

    delete _arg.absArg();
    delete _pipe;
    _exit(0) ;
  } else {
    // Client process - fork successul
    if (_verboseClient) ccoutD(Minimization) << "RooRealMPFE::initialize(" <<
	GetName() << ") successfully forked server process " <<
	    _pipe->pidOtherEnd() << endl ;
    _state = Client ;
    _calcInProgress = kFALSE ;
  }
#endif // _WIN32
}

////////////////////////////////////////////////////////////////////////////////
/// Set the cpu affinity of the server process to a specific cpu.

void RooRealMPFE::setCpuAffinity(int cpu) {
  Message msg = SetCpuAffinity;
  *_pipe << msg << cpu;
}


////////////////////////////////////////////////////////////////////////////////
/// Pipe stream operators for timing type.
namespace RooFit {
  using WallClock = std::chrono::system_clock;
  using TimePoint = WallClock::time_point;
  using Duration = WallClock::duration;

  BidirMMapPipe& BidirMMapPipe::operator<< (const TimePoint& wall) {
    Duration::rep const ns = wall.time_since_epoch().count();
    write(&ns, sizeof(ns));
    return *this;
  }

  BidirMMapPipe& BidirMMapPipe::operator>> (TimePoint& wall) {
    Duration::rep ns;
    read(&ns, sizeof(ns));

    Duration const d(ns);
    wall = TimePoint(d);

    return *this;
  }
}


////////////////////////////////////////////////////////////////////////////////
/// Convenience function to find a named component of the PDF.
RooAbsArg* RooRealMPFE::_findComponent(std::string name) {
  if (!_components) {
    const RooAbsReal& true_arg = _arg.arg();
    _components = true_arg.getComponents();
  }

  RooAbsArg* component = _components->find(name.c_str());

  cout << "component: " << component <<endl << endl;
  cout << "components size: " << _components->getSize() << endl << endl;

  RooFIter iter = _components->fwdIterator();
  RooAbsArg* node;
  int i = 0;
  while((node = iter.next())) {
    cout << "name of component " << i << ": " << node->GetName() << endl << endl;
    ++i;
  }
  return component;
}


////////////////////////////////////////////////////////////////////////////////
/// Server loop of remote processes. This function will return
/// only when an incoming TERMINATE message is received.

void RooRealMPFE::serverLoop() {
#ifndef _WIN32
  RooJsonListFile timing_outfile;
  RooWallTimer timer;

  if (RooTrace::timing_flag == 9) {
    stringstream filename_ss;
    filename_ss << "timing_RRMPFE_serverloop_while_p" << getpid() << ".json";
    timing_outfile.open(filename_ss.str().c_str());
    std::string names[3] = {"RRMPFE_serverloop_while_wall_s", "pid", "ppid"};
    timing_outfile.set_member_names(names, names + 3);
  }

  if (RooTrace::timing_flag == 8) {
    stringstream filename_ss;
    filename_ss << "timing_RRMPFE_serverloop_p" << getpid() << ".json";
    timing_outfile.open(filename_ss.str().c_str());
    std::string names[3] = {"RRMPFE_serverloop_wall_s", "pid", "ppid"};
    timing_outfile.set_member_names(names, names + 3);
    timer.start();
  }

  int msg;

  Int_t idx, index, numErrors;
  Double_t value;
  Bool_t isConst;

  clearEvalErrorLog();

  while (*_pipe && !_pipe->eof()) {
    if (RooTrace::timing_flag == 9) {
      timer.start();
    }
    *_pipe >> msg;
    if (Terminate == msg) {
      if (_verboseServer)
        cout << "RooRealMPFE::serverLoop(" << GetName()
             << ") IPC fromClient> Terminate" << endl;
      // send terminate acknowledged to client
      *_pipe << msg << BidirMMapPipe::flush;
      break;
    }

    switch (msg) {
      case SendReal: {
        *_pipe >> idx >> value >> isConst;
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> SendReal [" << idx << "]=" << value << endl;
        RooRealVar *rvar = (RooRealVar *) _vars.at(idx);
        rvar->setVal(value);
        if (rvar->isConstant() != isConst) {
          rvar->setConstant(isConst);
        }
      }
        break;

      case SendCat: {
        *_pipe >> idx >> index;
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> SendCat [" << idx << "]=" << index << endl;
        ((RooCategory *) _vars.at(idx))->setIndex(index);
      }
        break;

      case Calculate:
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> Calculate" << endl;
        _value = _arg;
        break;

      case CalculateNoOffset:
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> Calculate" << endl;

        RooAbsReal::setHideOffset(kFALSE);
        _value = _arg;
        RooAbsReal::setHideOffset(kTRUE);
        break;

      case Retrieve: {
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> Retrieve" << endl;
        msg = ReturnValue;
        numErrors = numEvalErrors();
        *_pipe << msg << _value << getCarry() << numErrors;

        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC toClient> ReturnValue " << _value << " NumError " << numErrors << endl;

        if (numErrors) {
          // Loop over errors
          std::string objidstr;
          {
            ostringstream oss2;
            // Format string with object identity as this cannot be evaluated on the other side
            oss2 << "PID" << gSystem->GetPid() << "/";
            printStream(oss2, kName | kClassName | kArgs, kInline);
            objidstr = oss2.str();
          }
          std::map<const RooAbsArg *, pair<string, list < EvalError> > > ::const_iterator
          iter = evalErrorIter();
          const RooAbsArg *ptr = 0;
          for (int i = 0; i < numEvalErrorItems(); ++i) {
            list<EvalError>::const_iterator iter2 = iter->second.second.begin();
            for (; iter->second.second.end() != iter2; ++iter2) {
              ptr = iter->first;
              *_pipe << ptr << iter2->_msg << iter2->_srvval << objidstr;
              if (_verboseServer)
                cout << "RooRealMPFE::serverLoop(" << GetName()
                     << ") IPC toClient> sending error log Arg " << iter->first << " Msg " << iter2->_msg << endl;
            }
          }
          // let other end know that we're done with the list of errors
          ptr = 0;
          *_pipe << ptr;
          // Clear error list on local side
          clearEvalErrorLog();
        }
        *_pipe << BidirMMapPipe::flush;
      }
        break;

      case ConstOpt: {
        Bool_t doTrack;
        int code;
        *_pipe >> code >> doTrack;
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> ConstOpt " << code << " doTrack = " << (doTrack ? "T" : "F") << endl;
        ((RooAbsReal &) _arg.arg()).constOptimizeTestStatistic(static_cast<RooAbsArg::ConstOpCode>(code), doTrack);
        break;
      }

      case Verbose: {
        Bool_t flag;
        *_pipe >> flag;
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> Verbose " << (flag ? 1 : 0) << endl;
        _verboseServer = flag;
      }
        break;


      case ApplyNLLW2: {
        Bool_t flag;
        *_pipe >> flag;
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> ApplyNLLW2 " << (flag ? 1 : 0) << endl;

        // Do application of weight-squared here
        doApplyNLLW2(flag);
      }
        break;

      case EnableOffset: {
        Bool_t flag;
        *_pipe >> flag;
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> EnableOffset " << (flag ? 1 : 0) << endl;

        // Enable likelihoof offsetting here
        ((RooAbsReal &) _arg.arg()).enableOffsetting(flag);
      }
        break;

      case LogEvalError: {
        int iflag2;
        *_pipe >> iflag2;
        RooAbsReal::ErrorLoggingMode flag2 = static_cast<RooAbsReal::ErrorLoggingMode>(iflag2);
        RooAbsReal::setEvalErrorLoggingMode(flag2);
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> LogEvalError flag = " << flag2 << endl;
      }
        break;


      case SetCpuAffinity: {
        int cpu;
        *_pipe >> cpu;

        #if defined(__APPLE__)
        std::cout << "WARNING: CPU affinity cannot be set on macOS, continuing..." << std::endl;
        #elif defined(_WIN32)
        std::cout << "WARNING: CPU affinity setting not implemented on Windows, continuing..." << std::endl;
        #else
        cpu_set_t mask;
        // zero all bits in mask
        CPU_ZERO(&mask);
        // set correct bit
        CPU_SET(cpu, &mask);
        /* sched_setaffinity returns 0 in success */

        if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {
          std::cout << "WARNING: Could not set CPU affinity, continuing..." << std::endl;
        } else {
          std::cout << "CPU affinity set to cpu " << cpu << " in server process " << getpid() << std::endl;
        }
        #endif

        break;
      }


      case EnableTimingRATS: {
        setAttribute("timing_on");

        break;
      }


      case DisableTimingRATS: {
        setAttribute("timing_on", kFALSE);

        break;
      }


      case EnableTimingNamedAbsArg: {
        std::string name;
        *_pipe >> name;

        RooAbsArg * absArg = _findComponent(name);
        if (absArg) {
          absArg->setAttribute("timing_on");
        } else {
          std::cout << "_findComponent for component " << name << " returned null pointer in process " << getpid() << "!" << std::endl;
          msg = Terminate;
        }

        break;
      }


      case DisableTimingNamedAbsArg: {
        std::string name;
        *_pipe >> name;

        _findComponent(name)->setAttribute("timing_on", kFALSE);

        break;
      }


      case EnableTimingNumInts: {
        // This must be done server-side, otherwise you have to copy all timing flags to server manually anyway
        // FIXME: make this more general than just RooAbsTestStatistic (when needed)
        dynamic_cast<RooAbsTestStatistic*>(_arg.absArg())->_setNumIntTimingInPdfs();
        break;
      }


      case DisableTimingNumInts: {
        dynamic_cast<RooAbsTestStatistic*>(_arg.absArg())->_setNumIntTimingInPdfs(kFALSE);
        break;
      }


      case MeasureCommunicationTime: {
        // Measure end time asap, since time of arrival at this case block is what we need to measure
        // communication overhead, i.e. time between sending message and corresponding action taken.
        // Defining the end time variable can reasonably be called overhead too, since many other
        // messages also define a piped-message-receiving variable.
        TimePoint comm_wallclock_begin, comm_wallclock_end;
        comm_wallclock_end = WallClock::now();

        *_pipe >> comm_wallclock_begin;

        std::cout << "client to server communication overhead timing:" << std::endl;
        std::cout << "comm_wallclock_begin: " << std::chrono::duration_cast<std::chrono::nanoseconds>(comm_wallclock_begin.time_since_epoch()).count() << std::endl;
        std::cout << "comm_wallclock_end: " << std::chrono::duration_cast<std::chrono::nanoseconds>(comm_wallclock_end.time_since_epoch()).count() << std::endl;

        double comm_wallclock_s = std::chrono::duration_cast<std::chrono::nanoseconds>(comm_wallclock_end - comm_wallclock_begin).count() / 1.e9;

        std::cout << "comm_wallclock (seconds): " << comm_wallclock_s << std::endl;

        comm_wallclock_begin = WallClock::now();
        *_pipe << comm_wallclock_begin << BidirMMapPipe::flush;

        break;
      }

      case RetrieveTimings: {
        Bool_t clear_timings;
        *_pipe >> clear_timings;
        *_pipe << static_cast<unsigned long>(RooTrace::objectTiming.size()) << BidirMMapPipe::flush;
        for (auto it = RooTrace::objectTiming.begin(); it != RooTrace::objectTiming.end(); ++it) {
          std::string name = it->first;
          double timing_s = it->second;
          *_pipe << name << timing_s << BidirMMapPipe::flush;
        }
        if (clear_timings == kTRUE) {
          RooTrace::objectTiming.clear();
        }

        break;
      }

      case GetPID: {
        *_pipe << getpid() << BidirMMapPipe::flush;
        break;
      }


      default:
        if (_verboseServer)
          cout << "RooRealMPFE::serverLoop(" << GetName()
               << ") IPC fromClient> Unknown message (code = " << msg << ")" << endl;
        break;
    }

    // end timing
    if (RooTrace::timing_flag == 9) {
      timer.stop();
      timing_outfile << timer.timing_s() << getpid() << getppid();
    }

    if (Terminate == msg) {
      if (_verboseServer)
        cout << "RooRealMPFE::serverLoop(" << GetName()
             << ") Terminate from inside loop itself" << endl;
      break;
    }

  }

  // end timing
  if (RooTrace::timing_flag == 8) {
    timer.stop();
    timing_outfile << timer.timing_s() << getpid() << getppid();
  }

#endif // _WIN32
}


////////////////////////////////////////////////////////////////////////////////

void RooRealMPFE::setTimingNumInts(Bool_t flag) {
  if (flag == kTRUE) {
    *_pipe << EnableTimingNumInts;
  } else {
    *_pipe << DisableTimingNumInts;
  }
}


////////////////////////////////////////////////////////////////////////////////
/// Client-side function that instructs server process to start
/// asynchronuous (re)calculation of function value. This function
/// returns immediately. The calculated value can be retrieved
/// using getVal()

void RooRealMPFE::calculate() const
{
  RooJsonListFile timing_outfile;
  RooWallTimer timer;

  // Start asynchronous calculation of arg value
  if (_state==Initialize) {
    //     cout << "RooRealMPFE::calculate(" << GetName() << ") initializing" << endl ;
    if (RooTrace::timing_flag == 7) {
      timing_outfile.open("timing_RRMPFE_calculate_initialize.json");
      std::string names[2] = {"RRMPFE_calculate_initialize_wall_s", "pid"};
      timing_outfile.set_member_names(names, names + 2);
      timer.start();
    }

    const_cast<RooRealMPFE*>(this)->initialize() ;

    if (RooTrace::timing_flag == 7) {
      timer.stop();
      timing_outfile << timer.timing_s() << getpid();
    }
  }

  // Inline mode -- Calculate value now
  if (_state==Inline) {
    //     cout << "RooRealMPFE::calculate(" << GetName() << ") performing Inline calculation NOW" << endl ;
    if (RooTrace::timing_flag == 7) {
      timing_outfile.open("timing_RRMPFE_calculate_inline.json");
      std::string names[2] = {"RRMPFE_calculate_inline_wall_s", "pid"};
      timing_outfile.set_member_names(names, names + 2);
      timer.start();
    }

    _value = _arg ;
    clearValueDirty() ;

    if (RooTrace::timing_flag == 7) {
      timer.stop();
      timing_outfile << timer.timing_s() << getpid();
    }
  }

#ifndef _WIN32
  // Compare current value of variables with saved values and send changes to server
  if (_state==Client) {
    // timing stuff
    if (RooTrace::timing_flag == 7) {
      timing_outfile.open("timing_RRMPFE_calculate_client.json");
      std::string names[2] = {"RRMPFE_calculate_client_wall_s", "pid"};
      timing_outfile.set_member_names(names, names + 2);
      timer.start();
    }

    if (RooTrace::timing_flag == 10) {
      _time_communication_overhead();
    }

    //     cout << "RooRealMPFE::calculate(" << GetName() << ") state is Client trigger remote calculation" << endl ;
    Int_t i(0) ;
    RooFIter viter = _vars.fwdIterator() ;
    RooFIter siter = _saveVars.fwdIterator() ;

    //for (i=0 ; i<_vars.getSize() ; i++) {
    RooAbsArg *var, *saveVar ;
    while((var = viter.next())) {
      saveVar = siter.next() ;

      //Bool_t valChanged = !(*var==*saveVar) ;
      Bool_t valChanged,constChanged  ;
      if (!_updateMaster) {
	valChanged = !var->isIdentical(*saveVar,kTRUE) ;
	constChanged = (var->isConstant() != saveVar->isConstant()) ;
	_valueChanged[i] = valChanged ;
	_constChanged[i] = constChanged ;
      } else {
	valChanged = _updateMaster->_valueChanged[i] ;
	constChanged = _updateMaster->_constChanged[i] ;
      }

      if ( valChanged || constChanged || _forceCalc) {
	//cout << "RooRealMPFE::calculate(" << GetName() << " variable " << var->GetName() << " changed " << endl ;
	if (_verboseClient) cout << "RooRealMPFE::calculate(" << GetName()
				 << ") variable " << _vars.at(i)->GetName() << " changed" << endl ;
	if (constChanged) {
	  ((RooRealVar*)saveVar)->setConstant(var->isConstant()) ;
	}
	saveVar->copyCache(var) ;

	// send message to server
	if (dynamic_cast<RooAbsReal*>(var)) {
	  int msg = SendReal ;
	  Double_t val = ((RooAbsReal*)var)->getVal() ;
	  Bool_t isC = var->isConstant() ;
	  *_pipe << msg << i << val << isC;

	  if (_verboseServer) cout << "RooRealMPFE::calculate(" << GetName()
				   << ") IPC toServer> SendReal [" << i << "]=" << val << (isC?" (Constant)":"") <<  endl ;
	} else if (dynamic_cast<RooAbsCategory*>(var)) {
	  int msg = SendCat ;
	  UInt_t idx = ((RooAbsCategory*)var)->getIndex() ;
	  *_pipe << msg << i << idx;
	  if (_verboseServer) cout << "RooRealMPFE::calculate(" << GetName()
				   << ") IPC toServer> SendCat [" << i << "]=" << idx << endl ;
	}
      }
      i++ ;
    }

    int msg = hideOffset() ? Calculate : CalculateNoOffset;
    *_pipe << msg;
    if (_verboseServer) cout << "RooRealMPFE::calculate(" << GetName()
			     << ") IPC toServer> Calculate " << endl ;

    // Clear dirty state and mark that calculation request was dispatched
    clearValueDirty() ;
    _calcInProgress = kTRUE ;
    _forceCalc = kFALSE ;

    msg = Retrieve ;
    *_pipe << msg << BidirMMapPipe::flush;
    if (_verboseServer) cout << "RooRealMPFE::evaluate(" << GetName()
			     << ") IPC toServer> Retrieve " << endl ;
    _retrieveDispatched = kTRUE ;

    // end timing
    if (RooTrace::timing_flag == 7) {
      timer.stop();
      timing_outfile << timer.timing_s() << getpid();
    }

  } else if (_state!=Inline) {
    cout << "RooRealMPFE::calculate(" << GetName()
	 << ") ERROR not in Client or Inline mode" << endl ;
  }


#endif // _WIN32
}




////////////////////////////////////////////////////////////////////////////////
/// If value needs recalculation and calculation has not beed started
/// with a call to calculate() start it now. This function blocks
/// until remote process has finished calculation and returns
/// remote value

Double_t RooRealMPFE::getValV(const RooArgSet* /*nset*/) const
{

  if (isValueDirty()) {
    // Cache is dirty, no calculation has been started yet
    //cout << "RooRealMPFE::getValF(" << GetName() << ") cache is dirty, caling calculate and evaluate" << endl ;
    calculate() ;
    _value = evaluate() ;
  } else if (_calcInProgress) {
    //cout << "RooRealMPFE::getValF(" << GetName() << ") calculation in progress, calling evaluate" << endl ;
    // Cache is clean and calculation is in progress
    _value = evaluate() ;
  } else {
    //cout << "RooRealMPFE::getValF(" << GetName() << ") cache is clean, doing nothing" << endl ;
    // Cache is clean and calculated value is in cache
  }

//   cout << "RooRealMPFE::getValV(" << GetName() << ") value = " << Form("%5.10f",_value) << endl ;
  return _value ;
}



////////////////////////////////////////////////////////////////////////////////
/// Send message to server process to retrieve output value
/// If error were logged use logEvalError() on remote side
/// transfer those errors to the local eval error queue.

Double_t RooRealMPFE::evaluate() const
{
  RooJsonListFile timing_outfile;
  RooWallTimer wtimer, wtimer_before, wtimer_retrieve, wtimer_after;
  RooCPUTimer ctimer, ctimer_before, ctimer_retrieve, ctimer_after;

  if (RooTrace::timing_flag == 4) {
    timing_outfile.open("timing_RRMPFE_evaluate_full.json");
    std::string names[2] = {"RRMPFE_evaluate_wall_s", "pid"};
    timing_outfile.set_member_names(names, names + 2);
    wtimer.start();
  }

  // Retrieve value of arg
  Double_t return_value = 0;
  if (_state==Inline) {
    return_value = _arg ;
  } else if (_state==Client) {
#ifndef _WIN32
    if (RooTrace::timing_flag == 5) {
      timing_outfile.open("timing_wall_RRMPFE_evaluate_client.json");
      std::string names[4] = {"time s", "cpu/wall", "segment", "pid"};
      timing_outfile.set_member_names(names, names + 4);
      wtimer.start();
      wtimer_before.start();
    }
    if (RooTrace::timing_flag == 6) {
      timing_outfile.open("timing_cpu_RRMPFE_evaluate_client.json");
      std::string names[4] = {"time s", "cpu/wall", "segment", "pid"};
      timing_outfile.set_member_names(names, names + 4);
      ctimer.start();
      ctimer_before.start();
    }

    bool needflush = false;
    int msg_i;
    Message msg;
    Double_t value;

    // If current error loggin state is not the same as remote state
    // update the remote state
    if (evalErrorLoggingMode() != _remoteEvalErrorLoggingState) {
      msg = LogEvalError ;
      RooAbsReal::ErrorLoggingMode flag = evalErrorLoggingMode() ;
      *_pipe << static_cast<int>(msg) << flag;
      needflush = true;
      _remoteEvalErrorLoggingState = evalErrorLoggingMode() ;
    }

    if (!_retrieveDispatched) {
      msg = Retrieve ;
      *_pipe << static_cast<int>(msg);
      needflush = true;
      if (_verboseServer) cout << "RooRealMPFE::evaluate(" << GetName()
			       << ") IPC toServer> Retrieve " << endl ;
    }
    if (needflush) *_pipe << BidirMMapPipe::flush;
    _retrieveDispatched = kFALSE ;


    Int_t numError;

    if (RooTrace::timing_flag == 5) {
      wtimer_before.stop();
      wtimer_retrieve.start();
    }
    if (RooTrace::timing_flag == 6) {
      ctimer_before.stop();
      ctimer_retrieve.start();
    }

    *_pipe >> msg_i >> value >> _evalCarry >> numError;
    msg = static_cast<Message>(msg_i);

    if (RooTrace::timing_flag == 5) {
      wtimer_retrieve.stop();
      wtimer_after.start();
    }
    if (RooTrace::timing_flag == 6) {
      ctimer_retrieve.stop();
      ctimer_after.start();
    }

    if (msg!=ReturnValue) {
      cout << "RooRealMPFE::evaluate(" << GetName()
	   << ") ERROR: unexpected message from server process: " << msg << endl ;
      return 0 ;
    }
    if (_verboseServer) cout << "RooRealMPFE::evaluate(" << GetName()
			     << ") IPC fromServer> ReturnValue " << value << endl ;

    if (_verboseServer) cout << "RooRealMPFE::evaluate(" << GetName()
			     << ") IPC fromServer> NumErrors " << numError << endl ;
    if (numError) {
      // Retrieve remote errors and feed into local error queue
      char *msgbuf1 = 0, *msgbuf2 = 0, *msgbuf3 = 0;
      RooAbsArg *ptr = 0;
      while (true) {
	*_pipe >> ptr;
	if (!ptr) break;
	*_pipe >> msgbuf1 >> msgbuf2 >> msgbuf3;
	if (_verboseServer) cout << "RooRealMPFE::evaluate(" << GetName()
	  << ") IPC fromServer> retrieving error log Arg " << ptr << " Msg " << msgbuf1 << endl ;

	logEvalError(reinterpret_cast<RooAbsReal*>(ptr),msgbuf3,msgbuf1,msgbuf2) ;
      }
      std::free(msgbuf1);
      std::free(msgbuf2);
      std::free(msgbuf3);
    }

    // Mark end of calculation in progress
    _calcInProgress = kFALSE ;
    return_value = value ;


    if (RooTrace::timing_flag == 5) {
      wtimer_after.stop();
      wtimer.stop();

      timing_outfile << wtimer.timing_s()           << "wall" << "all" << getpid();
      timing_outfile << wtimer_before.timing_s()    << "wall" << "before_retrieve" << getpid();
      timing_outfile << wtimer_retrieve.timing_s()  << "wall" << "retrieve" << getpid();
      timing_outfile << wtimer_after.timing_s()     << "wall" << "after_retrieve" << getpid();
    }

    if (RooTrace::timing_flag == 6) {
      ctimer_after.stop();
      ctimer.stop();

      timing_outfile << ctimer.timing_s()           << "cpu" << "all" << getpid();
      timing_outfile << ctimer_before.timing_s()    << "cpu" << "before_retrieve" << getpid();
      timing_outfile << ctimer_retrieve.timing_s()  << "cpu" << "retrieve" << getpid();
      timing_outfile << ctimer_after.timing_s()     << "cpu" << "after_retrieve" << getpid();
    }


#endif // _WIN32
  }

  if (RooTrace::timing_flag == 4) {
    wtimer.stop();
    timing_outfile << wtimer.timing_s() << getpid();
  }

  return return_value;
}



////////////////////////////////////////////////////////////////////////////////
/// Terminate remote server process and return front-end class
/// to standby mode. Calls to calculate() or evaluate() after
/// this call will automatically recreated the server process.

void RooRealMPFE::standby()
{
#ifndef _WIN32
  if (_state==Client) {
    if (_pipe->good()) {
      // Terminate server process ;
      if (_verboseServer) cout << "RooRealMPFE::standby(" << GetName()
	<< ") IPC toServer> Terminate " << endl;
      int msg = Terminate;
      *_pipe << msg << BidirMMapPipe::flush;
      // read handshake
      msg = 0;
      *_pipe >> msg;
      if (Terminate != msg || 0 != _pipe->close()) {
	std::cerr << "In " << __func__ << "(" << __FILE__ ", " << __LINE__ <<
	  "): Server shutdown failed." << std::endl;
      }
    } else {
      if (_verboseServer) {
	std::cerr << "In " << __func__ << "(" << __FILE__ ", " <<
	  __LINE__ << "): Pipe has already shut down, not sending "
	  "Terminate to server." << std::endl;
      }
    }
    // Close pipes
    delete _pipe;
    _pipe = 0;

    // Revert to initialize state
    _state = Initialize;
  }
#endif // _WIN32
}



////////////////////////////////////////////////////////////////////////////////
/// Intercept call to optimize constant term in test statistics
/// and forward it to object on server side.

void RooRealMPFE::constOptimizeTestStatistic(ConstOpCode opcode, Bool_t doAlsoTracking)
{
#ifndef _WIN32
  if (_state==Client) {

    int msg = ConstOpt ;
    int op = opcode;
    *_pipe << msg << op << doAlsoTracking;
    if (_verboseServer) cout << "RooRealMPFE::constOptimize(" << GetName()
			     << ") IPC toServer> ConstOpt " << opcode << endl ;

    initVars() ;
  }
#endif // _WIN32

  if (_state==Inline) {
    ((RooAbsReal&)_arg.arg()).constOptimizeTestStatistic(opcode,doAlsoTracking) ;
  }
}



////////////////////////////////////////////////////////////////////////////////
/// Control verbose messaging related to inter process communication
/// on both client and server side

void RooRealMPFE::setVerbose(Bool_t clientFlag, Bool_t serverFlag)
{
#ifndef _WIN32
  if (_state==Client) {
    int msg = Verbose ;
    *_pipe << msg << serverFlag;
    if (_verboseServer) cout << "RooRealMPFE::setVerbose(" << GetName()
			     << ") IPC toServer> Verbose " << (serverFlag?1:0) << endl ;
  }
#endif // _WIN32
  _verboseClient = clientFlag ; _verboseServer = serverFlag ;
}


////////////////////////////////////////////////////////////////////////////////
/// Control verbose messaging related to inter process communication
/// on both client and server side

void RooRealMPFE::applyNLLWeightSquared(Bool_t flag)
{
#ifndef _WIN32
  if (_state==Client) {
    int msg = ApplyNLLW2 ;
    *_pipe << msg << flag;
    if (_verboseServer) cout << "RooRealMPFE::applyNLLWeightSquared(" << GetName()
			     << ") IPC toServer> ApplyNLLW2 " << (flag?1:0) << endl ;
  }
#endif // _WIN32
  doApplyNLLW2(flag) ;
}


////////////////////////////////////////////////////////////////////////////////

void RooRealMPFE::doApplyNLLW2(Bool_t flag)
{
  RooNLLVar* nll = dynamic_cast<RooNLLVar*>(_arg.absArg()) ;
  if (nll) {
    nll->applyWeightSquared(flag) ;
  }
}


////////////////////////////////////////////////////////////////////////////////
/// Control verbose messaging related to inter process communication
/// on both client and server side

void RooRealMPFE::enableOffsetting(Bool_t flag)
{
#ifndef _WIN32
  if (_state==Client) {
    int msg = EnableOffset ;
    *_pipe << msg << flag;
    if (_verboseServer) cout << "RooRealMPFE::enableOffsetting(" << GetName()
			     << ") IPC toServer> EnableOffset " << (flag?1:0) << endl ;
  }
#endif // _WIN32
  ((RooAbsReal&)_arg.arg()).enableOffsetting(flag) ;
}

std::map<std::string, double> RooRealMPFE::collectTimingsFromServer(Bool_t clear_timings) const {
  std::map<std::string, double> server_timings;

  *_pipe << RetrieveTimings << clear_timings << BidirMMapPipe::flush;

  unsigned long numTimings;
  *_pipe >> numTimings;

  for (unsigned long i = 0; i < numTimings; ++i) {
    std::string name;
    double timing_s;
    *_pipe >> name >> timing_s;
    server_timings.insert({name, timing_s});
  }

  return server_timings;
}

pid_t RooRealMPFE::getPIDFromServer() const {
  *_pipe << GetPID << BidirMMapPipe::flush;
  pid_t pid;
  *_pipe >> pid;
  return pid;
}

void RooRealMPFE::_time_communication_overhead() const {
  // test communication overhead timing
  TimePoint comm_wallclock_begin_c2s, comm_wallclock_begin_s2c, comm_wallclock_end_s2c;
  // ... from client to server
  comm_wallclock_begin_c2s = WallClock::now();
  *_pipe << MeasureCommunicationTime << comm_wallclock_begin_c2s << BidirMMapPipe::flush;
  // ... and from server to client
  *_pipe >> comm_wallclock_begin_s2c;
  comm_wallclock_end_s2c = WallClock::now();

  std::cout << "server to client communication overhead timing:" << std::endl;
  std::cout << "comm_wallclock_begin: " << std::chrono::duration_cast<std::chrono::nanoseconds>(comm_wallclock_begin_s2c.time_since_epoch()).count() << std::endl;
  std::cout << "comm_wallclock_end: " << std::chrono::duration_cast<std::chrono::nanoseconds>(comm_wallclock_end_s2c.time_since_epoch()).count() << std::endl;

  double comm_wallclock_s = std::chrono::duration_cast<std::chrono::nanoseconds>(comm_wallclock_end_s2c - comm_wallclock_begin_s2c).count() / 1.e9;

  std::cout << "comm_wallclock (seconds): " << comm_wallclock_s << std::endl;
}


std::ostream& operator<<(std::ostream& out, const RooRealMPFE::Message value){
  const char* s = 0;
#define PROCESS_VAL(p) case(p): s = #p; break;
  switch(value){
    PROCESS_VAL(RooRealMPFE::SendReal);
    PROCESS_VAL(RooRealMPFE::SendCat);
    PROCESS_VAL(RooRealMPFE::Calculate);
    PROCESS_VAL(RooRealMPFE::Retrieve);
    PROCESS_VAL(RooRealMPFE::ReturnValue);
    PROCESS_VAL(RooRealMPFE::Terminate);
    PROCESS_VAL(RooRealMPFE::ConstOpt);
    PROCESS_VAL(RooRealMPFE::Verbose);
    PROCESS_VAL(RooRealMPFE::LogEvalError);
    PROCESS_VAL(RooRealMPFE::ApplyNLLW2);
    PROCESS_VAL(RooRealMPFE::EnableOffset);
    PROCESS_VAL(RooRealMPFE::CalculateNoOffset);
    PROCESS_VAL(RooRealMPFE::SetCpuAffinity);
    PROCESS_VAL(RooRealMPFE::EnableTimingRATS);
    PROCESS_VAL(RooRealMPFE::DisableTimingRATS);
    PROCESS_VAL(RooRealMPFE::EnableTimingNamedAbsArg);
    PROCESS_VAL(RooRealMPFE::DisableTimingNamedAbsArg);
    PROCESS_VAL(RooRealMPFE::MeasureCommunicationTime);
    PROCESS_VAL(RooRealMPFE::RetrieveTimings);
    PROCESS_VAL(RooRealMPFE::EnableTimingNumInts);
    PROCESS_VAL(RooRealMPFE::DisableTimingNumInts);
    PROCESS_VAL(RooRealMPFE::GetPID);
    default: {
      s = "unknown Message!";
      break;
    }
  }
#undef PROCESS_VAL

  return out << s;
}


