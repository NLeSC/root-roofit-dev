/*****************************************************************************
 * Project: RooFit                                                           *
 * Package: RooFitCore                                                       *
 * @(#)root/roofitcore:$Id$
 * Authors:                                                                  *
 *   PB, Patrick Bos,     NL eScience Center, p.bos@esciencecenter.nl        *
 *   IP, Inti Pelupessy,  NL eScience Center, i.pelupessy@esciencecenter.nl  *
 *   VC, Vince Croft,     DIANA / NYU,        vincent.croft@cern.ch          *
 *                                                                           *
 * Redistribution and use in source and binary forms,                        *
 * with or without modification, are permitted according to the terms        *
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)             *
 *****************************************************************************/

#include <stdexcept> // runtime_error

#include <RooRandom.h>
#include <RooWorkspace.h>
#include <RooTimer.h>

#include <RooMinimizer.h>
#include <RooGradMinimizerFcn.h>
#include <RooFitResult.h>

#include <TestStatistics/LikelihoodGradientJob.h>
#include <TestStatistics/LikelihoodSerial.h>
#include <TestStatistics/RooUnbinnedL.h>
#include <RooFit/MultiProcess/JobManager.h>
#include <RooFit/MultiProcess/ProcessManager.h> // need to complete type for debugging

#include "gtest/gtest.h"
#include "../test_lib.h" // generate_1D_gaussian_pdf_nll


class LikelihoodGradientJob : public ::testing::TestWithParam<std::tuple<std::size_t, std::size_t>> {
};

TEST_P(LikelihoodGradientJob, Gaussian1D)
{
   // do a minimization, but now using GradMinimizer and its MP version

   RooMsgService::instance().setGlobalKillBelow(RooFit::ERROR);

   // parameters
   std::size_t NWorkers = std::get<0>(GetParam());
   //  RooFit::MultiProcess::NLLVarTask mp_task_mode = std::get<1>(GetParam());
   std::size_t seed = std::get<1>(GetParam());

   RooRandom::randomGenerator()->SetSeed(seed);

   RooWorkspace w = RooWorkspace();

   std::unique_ptr<RooAbsReal> nll;
   std::unique_ptr<RooArgSet> values;
   RooAbsPdf *pdf;
   RooDataSet *data;
   std::tie(nll, pdf, data, values) = generate_1D_gaussian_pdf_nll(w, 10000);
   // when c++17 support arrives, change to this:
   //  auto [nll, pdf, data, values] = generate_1D_gaussian_pdf_nll(w, 10000);
   RooRealVar *mu = w.var("mu");

   RooArgSet *savedValues = dynamic_cast<RooArgSet *>(values->snapshot());
   if (savedValues == nullptr) {
      throw std::runtime_error("params->snapshot() cannot be casted to RooArgSet!");
   }

   // --------

   std::unique_ptr<RooMinimizer> m0 = RooMinimizer::create<RooGradMinimizerFcn>(*nll);
   m0->setMinimizerType("Minuit2");

   m0->setStrategy(0);
//   m0->setVerbose(true);
   m0->setPrintLevel(-1);

   m0->migrad();

   RooFitResult *m0result = m0->lastMinuitFit();
   double minNll0 = m0result->minNll();
   double edm0 = m0result->edm();
   double mu0 = mu->getVal();
   double muerr0 = mu->getError();

   *values = *savedValues;

   RooFit::MultiProcess::JobManager::default_N_workers = NWorkers;
   auto likelihood = std::make_shared<RooFit::TestStatistics::RooUnbinnedL>(pdf, data, false, 0, 0);
   std::unique_ptr<RooMinimizer> m1 =
      RooMinimizer::create<RooFit::TestStatistics::LikelihoodSerial, RooFit::TestStatistics::LikelihoodGradientJob>(
         likelihood);
   m1->setMinimizerType("Minuit2");

   m1->setStrategy(0);
//   m1->setVerbose(true);
   m1->setPrintLevel(-1);

   m1->migrad();

   RooFitResult *m1result = m1->lastMinuitFit();
   double minNll1 = m1result->minNll();
   double edm1 = m1result->edm();
   double mu1 = mu->getVal();
   double muerr1 = mu->getError();

   EXPECT_EQ(minNll0, minNll1);
   EXPECT_EQ(mu0, mu1);
   EXPECT_EQ(muerr0, muerr1);
   EXPECT_EQ(edm0, edm1);

   m1->cleanup(); // necessary in tests to clean up global _theFitter
}

TEST(LikelihoodGradientJobDEBUGGING, DISABLED_Gaussian1DNominal)
{
   //  std::size_t NWorkers = 1;
   std::size_t seed = 1;

   RooRandom::randomGenerator()->SetSeed(seed);

   RooWorkspace w = RooWorkspace();

   std::unique_ptr<RooAbsReal> nll;
   std::unique_ptr<RooArgSet> _;
   RooAbsPdf *pdf;
   RooDataSet *data;
   std::tie(nll, pdf, data, _) = generate_1D_gaussian_pdf_nll(w, 10000);

   std::unique_ptr<RooMinimizer> m0 = RooMinimizer::create<RooGradMinimizerFcn>(*nll);
   m0->setMinimizerType("Minuit2");

   m0->setStrategy(0);
   m0->setPrintLevel(2);

   m0->migrad();
   m0->cleanup(); // necessary in tests to clean up global _theFitter
}

TEST(LikelihoodGradientJobDEBUGGING, Gaussian1DMultiProcess)
{
   std::size_t NWorkers = 1;
   std::size_t seed = 1;

   RooRandom::randomGenerator()->SetSeed(seed);

   RooWorkspace w = RooWorkspace();

   std::unique_ptr<RooAbsReal> nll;
   std::unique_ptr<RooArgSet> values;
   RooAbsPdf *pdf;
   RooDataSet *data;
   std::tie(nll, pdf, data, values) = generate_1D_gaussian_pdf_nll(w, 10000);

   RooFit::MultiProcess::JobManager::default_N_workers = NWorkers;
   auto likelihood = std::make_shared<RooFit::TestStatistics::RooUnbinnedL>(pdf, data, false, 0, 0);
   std::unique_ptr<RooMinimizer> m1 =
      RooMinimizer::create<RooFit::TestStatistics::LikelihoodSerial, RooFit::TestStatistics::LikelihoodGradientJob>(
         likelihood);
   m1->setMinimizerType("Minuit2");

   m1->setStrategy(0);
   m1->setPrintLevel(2);

   m1->migrad();
   m1->cleanup(); // necessary in tests to clean up global _theFitter
}

TEST(LikelihoodGradientJob, RepeatMigrad)
{
   // do multiple minimizations using MP::GradMinimizer, testing breakdown and rebuild

   RooMsgService::instance().setGlobalKillBelow(RooFit::ERROR);

   // parameters
   std::size_t NWorkers = 2;
   std::size_t seed = 5;

   RooRandom::randomGenerator()->SetSeed(seed);

   RooWorkspace w = RooWorkspace();

   std::unique_ptr<RooAbsReal> nll;
   std::unique_ptr<RooArgSet> values;
   RooAbsPdf *pdf;
   RooDataSet *data;
   std::tie(nll, pdf, data, values) = generate_1D_gaussian_pdf_nll(w, 10000);
   // when c++17 support arrives, change to this:
   //  auto [nll, values] = generate_1D_gaussian_pdf_nll(w, 10000);

   RooArgSet *savedValues = dynamic_cast<RooArgSet *>(values->snapshot());
   if (savedValues == nullptr) {
      throw std::runtime_error("params->snapshot() cannot be casted to RooArgSet!");
   }

   // --------

   RooFit::MultiProcess::JobManager::default_N_workers = NWorkers;
   auto likelihood = std::make_shared<RooFit::TestStatistics::RooUnbinnedL>(pdf, data, false, 0, 0);
   std::unique_ptr<RooMinimizer> m1 =
      RooMinimizer::create<RooFit::TestStatistics::LikelihoodSerial, RooFit::TestStatistics::LikelihoodGradientJob>(
         likelihood);

   m1->setMinimizerType("Minuit2");

   m1->setStrategy(0);
   m1->setPrintLevel(-1);

   std::cout << "... running migrad first time ..." << std::endl;
   m1->migrad();

//   std::cout << "... terminating JobManager instance ..." << std::endl;
//   RooFit::MultiProcess::JobManager::instance()->terminate();

   *values = *savedValues;

   std::cout << "... running migrad second time ..." << std::endl;
   m1->migrad();

   std::cout << "... cleaning up minimizer ..." << std::endl;
   m1->cleanup(); // necessary in tests to clean up global _theFitter
}

TEST_P(LikelihoodGradientJob, GaussianND)
{
   // do a minimization, but now using GradMinimizer and its MP version

   RooMsgService::instance().setGlobalKillBelow(RooFit::ERROR);

   // parameters
   std::size_t NWorkers = std::get<0>(GetParam());
   //  RooFit::MultiProcess::NLLVarTask mp_task_mode = std::get<1>(GetParam());
   std::size_t seed = std::get<1>(GetParam());

   unsigned int N = 4;

   RooRandom::randomGenerator()->SetSeed(seed);

   RooWorkspace w = RooWorkspace();

   std::unique_ptr<RooAbsReal> nll;
   std::unique_ptr<RooArgSet> values;
   RooAbsPdf *pdf;
   RooDataSet *data;
   std::tie(nll, pdf, data, values) = generate_ND_gaussian_pdf_nll(w, N, 1000);
   // when c++17 support arrives, change to this:
   //  auto [nll, all_values] = generate_ND_gaussian_pdf_nll(w, N, 1000);

   RooArgSet *savedValues = dynamic_cast<RooArgSet *>(values->snapshot());
   if (savedValues == nullptr) {
      throw std::runtime_error("params->snapshot() cannot be casted to RooArgSet!");
   }

   RooWallTimer wtimer;

   // --------

   std::unique_ptr<RooMinimizer> m0 = RooMinimizer::create<RooGradMinimizerFcn>(*nll);
   m0->setMinimizerType("Minuit2");

   m0->setStrategy(0);
   m0->setPrintLevel(-1);

   wtimer.start();
   m0->migrad();
   wtimer.stop();
   std::cout << "\nwall clock time RooGradMinimizer.migrad (NWorkers = " << NWorkers << ", seed = " << seed
             << "): " << wtimer.timing_s() << " s" << std::endl;

   RooFitResult *m0result = m0->lastMinuitFit();
   double minNll0 = m0result->minNll();
   double edm0 = m0result->edm();
   double mean0[N];
   double std0[N];
   for (unsigned ix = 0; ix < N; ++ix) {
      {
         std::ostringstream os;
         os << "m" << ix;
         mean0[ix] = dynamic_cast<RooRealVar *>(w.arg(os.str().c_str()))->getVal();
      }
      {
         std::ostringstream os;
         os << "s" << ix;
         std0[ix] = dynamic_cast<RooRealVar *>(w.arg(os.str().c_str()))->getVal();
      }
   }

   // --------

   *values = *savedValues;

   // --------

   RooFit::MultiProcess::JobManager::default_N_workers = NWorkers;
   auto likelihood = std::make_shared<RooFit::TestStatistics::RooUnbinnedL>(pdf, data, false, 0, 0);
   std::unique_ptr<RooMinimizer> m1 =
      RooMinimizer::create<RooFit::TestStatistics::LikelihoodSerial, RooFit::TestStatistics::LikelihoodGradientJob>(
         likelihood);
   m1->setMinimizerType("Minuit2");

   m1->setStrategy(0);
   m1->setPrintLevel(-1);

   wtimer.start();
   m1->migrad();
   wtimer.stop();
   std::cout << "wall clock time MP::GradMinimizer.migrad (NWorkers = " << NWorkers << ", seed = " << seed
             << "): " << wtimer.timing_s() << " s\n"
             << std::endl;

   RooFitResult *m1result = m1->lastMinuitFit();
   double minNll1 = m1result->minNll();
   double edm1 = m1result->edm();
   double mean1[N];
   double std1[N];
   for (unsigned ix = 0; ix < N; ++ix) {
      {
         std::ostringstream os;
         os << "m" << ix;
         mean1[ix] = dynamic_cast<RooRealVar *>(w.arg(os.str().c_str()))->getVal();
      }
      {
         std::ostringstream os;
         os << "s" << ix;
         std1[ix] = dynamic_cast<RooRealVar *>(w.arg(os.str().c_str()))->getVal();
      }
   }

   EXPECT_EQ(minNll0, minNll1);
   EXPECT_EQ(edm0, edm1);

   for (unsigned ix = 0; ix < N; ++ix) {
      EXPECT_EQ(mean0[ix], mean1[ix]);
      EXPECT_EQ(std0[ix], std1[ix]);
   }

   m1->cleanup(); // necessary in tests to clean up global _theFitter
}

INSTANTIATE_TEST_CASE_P(NworkersSeed, LikelihoodGradientJob,
                        ::testing::Combine(::testing::Values(1, 2, 3), // number of workers
                                           ::testing::Values(2, 3)));  // random seed
