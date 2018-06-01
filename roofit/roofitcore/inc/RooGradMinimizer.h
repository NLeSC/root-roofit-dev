/*****************************************************************************
 * Project: RooFit                                                           *
 * Package: RooFitCore                                                       *
 *    File: $Id$
 * Authors:                                                                  *
 *   PB, Patrick Bos,     NL eScience Center, p.bos@esciencecenter.nl        *
 *                                                                           *
 *                                                                           *
 * Redistribution and use in source and binary forms,                        *
 * with or without modification, are permitted according to the terms        *
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)             *
 *****************************************************************************/
#ifndef ROO_GRAD_MINIMIZER
#define ROO_GRAD_MINIMIZER

#include "RooMinimizer.h"

// Same as in RooMinimizer, RooGradMinimizerFcn must be forward declared here
// to avoid circular dependency problems. The RooGradMinimizerFcn.h include
// must then be done below this, otherwise RooGradMinimizerFcn.h has no
// definition of RooGradMinimizer when needed.
class RooGradMinimizerFcn;
using RooGradMinimizer = RooMinimizerTemplate<RooGradMinimizerFcn, RooFit::MinimizerType::Minuit2>;

#include "RooGradMinimizerFcn.h"

#endif
