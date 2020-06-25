/*
 * Project: RooFit
 * Authors:
 *   PB, Patrick Bos, Netherlands eScience Center, p.bos@esciencecenter.nl
 *
 * Copyright (c) 2016-2020, Netherlands eScience Center
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */
#ifndef ROOT_ROOFIT_TESTSTATISTICS_LikelihoodWrapper
#define ROOT_ROOFIT_TESTSTATISTICS_LikelihoodWrapper

#include <memory>  // shared_ptr
#include <string>
#include <Fit/ParameterSettings.h>
#include "Math/MinimizerOptions.h"
#include "RooArgSet.h"
#include "RooAbsArg.h"  // enum ConstOpCode

// forward declaration
class RooMinimizer;

namespace RooFit {
namespace TestStatistics {

// forward declaration
class RooAbsL;

class LikelihoodWrapper {
public:
   LikelihoodWrapper(std::shared_ptr<RooAbsL> likelihood, RooMinimizer* minimizer);
   virtual ~LikelihoodWrapper() = default;
   virtual LikelihoodWrapper* clone() const = 0;

   virtual void evaluate() = 0;
   virtual double return_result() const = 0;

   // synchronize minimizer settings with calculators in child classes
   virtual void synchronize_with_minimizer(const ROOT::Math::MinimizerOptions & options);
   virtual void synchronize_parameter_settings(const std::vector<ROOT::Fit::ParameterSettings> &parameter_settings);

   // necessary from MinuitFcnGrad to reach likelihood properties:
   RooArgSet* getParameters();
   void constOptimizeTestStatistic(RooAbsArg::ConstOpCode opcode, bool doAlsoTrackingOpt);

   double defaultErrorLevel() const;

   virtual std::string GetName() const;
   virtual std::string GetTitle() const;

   virtual bool is_offsetting() const;
   virtual void enable_offsetting(bool flag);

protected:
   std::shared_ptr<RooAbsL> likelihood;
   RooMinimizer* _minimizer;
};

}
}

#endif // ROOT_ROOFIT_TESTSTATISTICS_LikelihoodWrapper
