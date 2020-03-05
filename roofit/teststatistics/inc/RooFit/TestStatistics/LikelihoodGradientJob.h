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
#ifndef ROOT_ROOFIT_TESTSTATISTICS_LikelihoodGradientJob
#define ROOT_ROOFIT_TESTSTATISTICS_LikelihoodGradientJob

#include <RooFit/MultiProcess/Job.h>
#include <RooFit/TestStatistics/LikelihoodGradientWrapper.h>

namespace RooFit {
namespace TestStatistics {

class LikelihoodGradientJob : MultiProcess::Job, LikelihoodGradientWrapper {
};

}
}


#endif // ROOT_ROOFIT_LikelihoodGradientJob
